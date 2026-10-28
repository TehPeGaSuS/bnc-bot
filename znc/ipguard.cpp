/*
 * SPDX-FileCopyrightText: 2026-present TehPeGaSuS
 *
 * SPDX-License-Identifier: MIT
 *
 * ipguard - block ZNC accounts that connect from the same IP prefix.
 *
 * On every login the client's IP is compared with the IPs of every other
 * account: the ones connected right now, and each account's previous IPs seen
 * within the last "window" hours (24 by default). When a different account
 * matches on the same IPv4 address / IPv6 prefix (/32 and /64 by default),
 * both accounts are blocked: their clients are disconnected, their networks
 * are disconnected from IRC, and further logins are refused until an admin
 * runs "Unblock", which also reconnects the networks that were connecting.
 * Admin users, exempt users and exempt CIDRs are never checked. Every block
 * is announced to all admin users from *ipguard. Manual blocks are left to
 * the blockuser module.
 */

#include <znc/IRCNetwork.h>
#include <znc/User.h>
#include <znc/znc.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cstring>
#include <ctime>
#include <set>

using std::set;
using std::vector;

#define MESSAGE \
    t_s("Your account has been disabled. Contact your administrator.")

namespace {

const char* const BLOCK_PREFIX = "block:";
const char* const SEEN_PREFIX = "seen:";
const char* const EXEMPT_PREFIX = "exempt:";
const char* const SET_PREFIX = "set:";

// Most recent distinct IPs remembered per account
const size_t MAX_SEEN = 10;

struct Address {
    int family = 0;
    unsigned char bytes[16] = {};
};

// Parse an IP, treating IPv4-mapped IPv6 (::ffff:a.b.c.d) as IPv4
bool ParseIP(const CString& sText, Address& addr) {
    CString sIP = sText.Token(0, false, "%");
    if (inet_pton(AF_INET, sIP.c_str(), addr.bytes) == 1) {
        addr.family = AF_INET;
        return true;
    }

    if (inet_pton(AF_INET6, sIP.c_str(), addr.bytes) != 1) return false;

    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(addr.bytes, mapped, sizeof(mapped)) == 0) {
        memmove(addr.bytes, addr.bytes + 12, 4);
        memset(addr.bytes + 4, 0, 12);
        addr.family = AF_INET;
    } else {
        addr.family = AF_INET6;
    }

    return true;
}

void Mask(Address& addr, unsigned int uBits) {
    unsigned int uLen = addr.family == AF_INET ? 4 : 16;
    for (unsigned int i = 0; i < uLen; i++) {
        if (uBits >= 8) {
            uBits -= 8;
        } else {
            addr.bytes[i] &= (unsigned char)(0xff << (8 - uBits));
            uBits = 0;
        }
    }
}

bool SameNet(Address a, Address b, unsigned int uBits) {
    if (a.family != b.family) return false;
    Mask(a, uBits);
    Mask(b, uBits);
    return memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}

CString FormatAddress(const Address& addr) {
    char buf[INET6_ADDRSTRLEN];
    if (!inet_ntop(addr.family, addr.bytes, buf, sizeof(buf))) return "";
    return buf;
}

CString FormatTime(time_t t) {
    char buf[32];
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

}  // namespace

class CIPGuardMod : public CModule {
  public:
    MODCONSTRUCTOR(CIPGuardMod) {
        AddHelpCommand();
        AddCommand("Show", t_d("<user>"),
                   t_d("Show a user's current and recent IPs"),
                   [=](const CString& sLine) { OnShowCommand(sLine); });
        AddCommand("Shared", t_d("<user>"),
                   t_d("List accounts that shared an IP prefix with a user"),
                   [=](const CString& sLine) { OnSharedCommand(sLine); });
        AddCommand("List", "", t_d("List blocked users"),
                   [=](const CString& sLine) { OnListCommand(sLine); });
        AddCommand("Unblock", t_d("<user>"),
                   t_d("Unblock a user blocked for a shared IP"),
                   [=](const CString& sLine) { OnUnblockCommand(sLine); });
        AddCommand("Exempt", t_d("<user|cidr>"),
                   t_d("Never check a user or network"),
                   [=](const CString& sLine) { OnExemptCommand(sLine); });
        AddCommand("Unexempt", t_d("<user|cidr>"), t_d("Remove an exemption"),
                   [=](const CString& sLine) { OnUnexemptCommand(sLine); });
        AddCommand("Exempts", "", t_d("List exemptions"),
                   [=](const CString& sLine) { OnExemptsCommand(sLine); });
        AddCommand("Set", t_d("<setting> <value>"),
                   t_d("Change a setting (see Settings)"),
                   [=](const CString& sLine) { OnSetCommand(sLine); });
        AddCommand("Settings", "", t_d("Show settings"),
                   [=](const CString& sLine) { OnSettingsCommand(sLine); });
    }

    EModRet OnLoginAttempt(std::shared_ptr<CAuthBase> Auth) override {
        if (IsBlocked(Auth->GetUsername())) {
            Auth->RefuseLogin(MESSAGE);
            return HALT;
        }

        return CONTINUE;
    }

    void OnClientLogin() override {
        CUser* pUser = GetUser();
        CString sIP = GetClient()->GetRemoteIP();
        Address addr;
        if (!ParseIP(sIP, addr)) return;

        time_t now = time(nullptr);
        if (!IsExemptUser(pUser) && !IsExemptIP(addr)) {
            Check(pUser, sIP, addr, now);
        }

        RecordSeen(pUser->GetUsername(), sIP, now);
    }

    void OnClientDisconnect() override {
        // Refresh the last-seen time so the window counts from the
        // disconnect rather than the login
        if (GetUser() && GetClient()) {
            RecordSeen(GetUser()->GetUsername(), GetClient()->GetRemoteIP(),
                       time(nullptr));
        }
    }

    EModRet OnDeleteUser(CUser& User) override {
        DelNV(BLOCK_PREFIX + User.GetUsername());
        DelNV(SEEN_PREFIX + User.GetUsername());
        return CONTINUE;
    }

    void OnModCommand(const CString& sCommand) override {
        if (!GetUser()->IsAdmin()) {
            PutModule(t_s("Access denied"));
        } else {
            HandleCommand(sCommand);
        }
    }

  private:
    struct Seen {
        CString sIP;
        time_t tTime;
    };

    // A client of another account on the same prefix
    struct Match {
        CString sUser;
        CString sIP;
        bool bLive;
    };

    unsigned int PrefixBits(const Address& addr) {
        return addr.family == AF_INET ? GetSetting("v4prefix").ToUInt()
                                      : GetSetting("v6prefix").ToUInt();
    }

    CString PrefixString(Address addr) {
        unsigned int uBits = PrefixBits(addr);
        Mask(addr, uBits);
        return FormatAddress(addr) + "/" + CString(uBits);
    }

    void Check(CUser* pUser, const CString& sIP, const Address& addr,
               time_t now) {
        vector<Match> vMatches = FindShared(pUser->GetUsername(), addr,
                                            GetSetting("window").ToUInt() *
                                                3600, now);
        if (vMatches.empty()) return;

        set<CString> ssUsers = {pUser->GetUsername()};
        VCString vsWho = {pUser->GetUsername() + " (" + sIP + ")"};
        for (const Match& m : vMatches) {
            if (ssUsers.insert(m.sUser).second) {
                vsWho.push_back(m.sUser + " (" + m.sIP +
                                (m.bLive ? "" : ", recently") + ")");
            }
        }

        CString sReason = "shared " + PrefixString(addr);
        for (const CString& sUser : ssUsers) {
            Block(sUser, sReason);
        }

        NotifyAdmins(t_f("Blocked {1}: {2}")(
            CString(", ").Join(vsWho.begin(), vsWho.end()), sReason));
    }

    // Other non-exempt accounts on addr's prefix: connected now, or (with a
    // window) seen within it. A zero window only considers live clients.
    vector<Match> FindShared(const CString& sUser, const Address& addr,
                             time_t tWindow, time_t now, bool bAll = false) {
        vector<Match> vMatches;
        unsigned int uBits = PrefixBits(addr);

        for (const auto& it : CZNC::Get().GetUserMap()) {
            CUser* pOther = it.second;
            if (pOther->GetUsername().Equals(sUser)) continue;
            if (!bAll && (IsExemptUser(pOther) ||
                          IsBlocked(pOther->GetUsername()))) {
                continue;
            }

            for (CClient* pClient : pOther->GetAllClients()) {
                Address other;
                CString sOtherIP = pClient->GetRemoteIP();
                if (ParseIP(sOtherIP, other) &&
                    (bAll || !IsExemptIP(other)) &&
                    SameNet(addr, other, uBits)) {
                    vMatches.push_back({pOther->GetUsername(), sOtherIP, true});
                }
            }

            if (!tWindow && !bAll) continue;

            for (const Seen& seen : GetSeen(pOther->GetUsername())) {
                Address other;
                if ((bAll || now - seen.tTime <= tWindow) &&
                    ParseIP(seen.sIP, other) &&
                    (bAll || !IsExemptIP(other)) &&
                    SameNet(addr, other, uBits)) {
                    vMatches.push_back(
                        {pOther->GetUsername(), seen.sIP, false});
                }
            }
        }

        return vMatches;
    }

    void Block(const CString& sUser, const CString& sReason) {
        CUser* pUser = CZNC::Get().FindUser(sUser);
        if (!pUser) return;

        for (CClient* pClient : pUser->GetAllClients()) {
            pClient->PutStatusNotice(MESSAGE);
            pClient->Close(Csock::CLT_AFTERWRITE);
        }

        // Remember which networks were connecting so Unblock can restore
        // exactly those
        VCString vsNetworks;
        for (CIRCNetwork* pNetwork : pUser->GetNetworks()) {
            if (pNetwork->GetIRCConnectEnabled()) {
                vsNetworks.push_back(pNetwork->GetName());
                pNetwork->SetIRCConnectEnabled(false);
            }
        }

        if (!IsBlocked(pUser->GetUsername())) {
            SetNV(BLOCK_PREFIX + pUser->GetUsername(),
                  CString(time(nullptr)) + " " +
                      CString(",").Join(vsNetworks.begin(),
                                        vsNetworks.end()) +
                      " " + sReason);
        }
    }

    bool IsBlocked(const CString& sUser) {
        return FindNV(BLOCK_PREFIX + sUser) != EndNV();
    }

    bool IsExemptUser(CUser* pUser) {
        return pUser->IsAdmin() ||
               FindNV(EXEMPT_PREFIX + pUser->GetUsername()) != EndNV();
    }

    bool IsExemptIP(const Address& addr) {
        for (auto it = BeginNV(); it != EndNV(); ++it) {
            if (!it->first.StartsWith(EXEMPT_PREFIX)) continue;
            CString sEntry = it->first.substr(strlen(EXEMPT_PREFIX));
            Address net;
            unsigned int uBits;
            if (ParseCIDR(sEntry, net, uBits) && SameNet(addr, net, uBits)) {
                return true;
            }
        }

        return false;
    }

    static bool ParseCIDR(const CString& sText, Address& net,
                          unsigned int& uBits) {
        if (!ParseIP(sText.Token(0, false, "/"), net)) return false;
        unsigned int uMax = net.family == AF_INET ? 32 : 128;
        CString sBits = sText.Token(1, false, "/");
        uBits = sBits.empty() ? uMax : sBits.ToUInt();
        return uBits <= uMax;
    }

    vector<Seen> GetSeen(const CString& sUser) {
        vector<Seen> vSeen;
        VCString vsEntries;
        GetNV(SEEN_PREFIX + sUser).Split(" ", vsEntries, false);
        for (const CString& sEntry : vsEntries) {
            vSeen.push_back({sEntry.Token(0, false, ","),
                             (time_t)sEntry.Token(1, false, ",").ToLongLong()});
        }

        return vSeen;
    }

    void RecordSeen(const CString& sUser, const CString& sIP, time_t now) {
        time_t tKeep = GetSetting("history").ToUInt() * 86400;
        VCString vsEntries = {sIP + "," + CString((long long)now)};
        for (const Seen& seen : GetSeen(sUser)) {
            if (seen.sIP != sIP && now - seen.tTime <= tKeep &&
                vsEntries.size() < MAX_SEEN) {
                vsEntries.push_back(seen.sIP + "," +
                                    CString((long long)seen.tTime));
            }
        }

        SetNV(SEEN_PREFIX + sUser,
              CString(" ").Join(vsEntries.begin(), vsEntries.end()));
    }

    void NotifyAdmins(const CString& sMessage) {
        for (const auto& it : CZNC::Get().GetUserMap()) {
            if (it.second->IsAdmin()) {
                it.second->PutModule(GetModName(), sMessage);
            }
        }
    }

    // name -> (default, description)
    static const std::map<CString, std::pair<CString, CString>>& Settings() {
        static const std::map<CString, std::pair<CString, CString>> settings =
            {
                {"window",
                 {"24", "Hours an account's previous IPs still count (0 = "
                        "only accounts connected right now)"}},
                {"history",
                 {"30", "Days of IP history kept per account (Show/Shared)"}},
                {"v4prefix", {"32", "IPv4 prefix length treated as one IP"}},
                {"v6prefix", {"64", "IPv6 prefix length treated as one IP"}},
            };
        return settings;
    }

    CString GetSetting(const CString& sName) {
        auto it = FindNV(SET_PREFIX + sName);
        return it != EndNV() ? it->second : Settings().at(sName).first;
    }

    CUser* FindUserOrComplain(const CString& sUser) {
        if (sUser.empty()) return nullptr;
        CUser* pUser = CZNC::Get().FindUser(sUser);
        if (!pUser) PutModule(t_f("No such user: {1}")(sUser));
        return pUser;
    }

    void OnShowCommand(const CString& sLine) {
        CString sUser = sLine.Token(1);
        if (sUser.empty()) {
            PutModule(t_s("Usage: Show <user>"));
            return;
        }

        CUser* pUser = FindUserOrComplain(sUser);
        if (!pUser) return;

        auto it = FindNV(BLOCK_PREFIX + pUser->GetUsername());
        if (it != EndNV()) {
            PutModule(t_f("{1} is blocked since {2}: {3}")(
                pUser->GetUsername(),
                FormatTime((time_t)it->second.Token(0).ToLongLong()),
                it->second.Token(2, true)));
        }

        CTable Table;
        Table.AddColumn(t_s("IP"));
        Table.AddColumn(t_s("Prefix"));
        Table.AddColumn(t_s("Last seen"));
        for (CClient* pClient : pUser->GetAllClients()) {
            Address addr;
            Table.AddRow();
            Table.SetCell(t_s("IP"), pClient->GetRemoteIP());
            if (ParseIP(pClient->GetRemoteIP(), addr)) {
                Table.SetCell(t_s("Prefix"), PrefixString(addr));
            }
            Table.SetCell(t_s("Last seen"), t_s("connected"));
        }

        for (const Seen& seen : GetSeen(pUser->GetUsername())) {
            Address addr;
            Table.AddRow();
            Table.SetCell(t_s("IP"), seen.sIP);
            if (ParseIP(seen.sIP, addr)) {
                Table.SetCell(t_s("Prefix"), PrefixString(addr));
            }
            Table.SetCell(t_s("Last seen"), FormatTime(seen.tTime));
        }

        if (Table.empty()) {
            PutModule(t_f("No IPs known for {1}")(pUser->GetUsername()));
        } else {
            PutModule(Table);
        }
    }

    void OnSharedCommand(const CString& sLine) {
        CString sUser = sLine.Token(1);
        if (sUser.empty()) {
            PutModule(t_s("Usage: Shared <user>"));
            return;
        }

        CUser* pUser = FindUserOrComplain(sUser);
        if (!pUser) return;

        // Every IP of this user (live and remembered) against every IP of
        // everyone else, ignoring exemptions and the window
        set<CString> ssIPs;
        for (CClient* pClient : pUser->GetAllClients()) {
            ssIPs.insert(pClient->GetRemoteIP());
        }
        for (const Seen& seen : GetSeen(pUser->GetUsername())) {
            ssIPs.insert(seen.sIP);
        }

        CTable Table;
        Table.AddColumn(t_s("User"));
        Table.AddColumn(t_s("Their IP"));
        Table.AddColumn(t_s("Prefix"));
        set<std::pair<CString, CString>> ssSeen;
        time_t now = time(nullptr);
        for (const CString& sIP : ssIPs) {
            Address addr;
            if (!ParseIP(sIP, addr)) continue;
            for (const Match& m :
                 FindShared(pUser->GetUsername(), addr, 0, now, true)) {
                if (!ssSeen.insert({m.sUser, m.sIP}).second) continue;
                Table.AddRow();
                Table.SetCell(t_s("User"), m.sUser);
                Table.SetCell(t_s("Their IP"), m.sIP);
                Table.SetCell(t_s("Prefix"), PrefixString(addr));
            }
        }

        if (Table.empty()) {
            PutModule(t_f("No other account shared an IP prefix with {1}")(
                pUser->GetUsername()));
        } else {
            PutModule(Table);
        }
    }

    void OnListCommand(const CString& sLine) {
        CTable Table;
        Table.AddColumn(t_s("User"));
        Table.AddColumn(t_s("Since"));
        Table.AddColumn(t_s("Reason"));
        for (auto it = BeginNV(); it != EndNV(); ++it) {
            if (!it->first.StartsWith(BLOCK_PREFIX)) continue;
            Table.AddRow();
            Table.SetCell(t_s("User"),
                          it->first.substr(strlen(BLOCK_PREFIX)));
            Table.SetCell(t_s("Since"),
                          FormatTime((time_t)it->second.Token(0).ToLongLong()));
            Table.SetCell(t_s("Reason"), it->second.Token(2, true));
        }

        if (Table.empty()) {
            PutModule(t_s("No users are blocked"));
        } else {
            PutModule(Table);
        }
    }

    void OnUnblockCommand(const CString& sLine) {
        CString sUser = sLine.Token(1);
        if (sUser.empty()) {
            PutModule(t_s("Usage: Unblock <user>"));
            return;
        }

        auto it = FindNV(BLOCK_PREFIX + sUser);
        if (it == EndNV()) {
            PutModule(t_f("{1} is not blocked")(sUser));
            return;
        }

        VCString vsNetworks;
        it->second.Token(1).Split(",", vsNetworks, false);
        DelNV(it->first);

        CUser* pUser = CZNC::Get().FindUser(sUser);
        if (pUser) {
            for (const CString& sNetwork : vsNetworks) {
                CIRCNetwork* pNetwork = pUser->FindNetwork(sNetwork);
                if (pNetwork) pNetwork->SetIRCConnectEnabled(true);
            }
        }

        PutModule(t_f("Unblocked {1}")(sUser));
    }

    void OnExemptCommand(const CString& sLine) {
        CString sEntry = sLine.Token(1);
        if (sEntry.empty()) {
            PutModule(t_s("Usage: Exempt <user|cidr>"));
            return;
        }

        Address net;
        unsigned int uBits;
        if (ParseCIDR(sEntry, net, uBits)) {
            Mask(net, uBits);
            sEntry = FormatAddress(net) + "/" + CString(uBits);
        } else if (!FindUserOrComplain(sEntry)) {
            return;
        }

        SetNV(EXEMPT_PREFIX + sEntry, "");
        PutModule(t_f("Exempted {1}")(sEntry));
    }

    void OnUnexemptCommand(const CString& sLine) {
        CString sEntry = sLine.Token(1);
        Address net;
        unsigned int uBits;
        if (ParseCIDR(sEntry, net, uBits)) {
            Mask(net, uBits);
            sEntry = FormatAddress(net) + "/" + CString(uBits);
        }

        if (DelNV(EXEMPT_PREFIX + sEntry)) {
            PutModule(t_f("Removed exemption for {1}")(sEntry));
        } else {
            PutModule(t_f("{1} is not exempt")(sEntry));
        }
    }

    void OnExemptsCommand(const CString& sLine) {
        VCString vsEntries;
        for (auto it = BeginNV(); it != EndNV(); ++it) {
            if (it->first.StartsWith(EXEMPT_PREFIX)) {
                vsEntries.push_back(it->first.substr(strlen(EXEMPT_PREFIX)));
            }
        }

        if (vsEntries.empty()) {
            PutModule(t_s("Nothing is exempt (admin users always are)"));
        } else {
            PutModule(t_f("Exempt (plus all admin users): {1}")(
                CString(", ").Join(vsEntries.begin(), vsEntries.end())));
        }
    }

    void OnSetCommand(const CString& sLine) {
        CString sName = sLine.Token(1).AsLower();
        CString sValue = sLine.Token(2);
        auto it = Settings().find(sName);
        if (it == Settings().end() || sValue.empty() ||
            sValue.find_first_not_of("0123456789") != CString::npos) {
            PutModule(t_s("Usage: Set <setting> <number> (see Settings)"));
            return;
        }

        unsigned int uValue = sValue.ToUInt();
        if ((sName == "v4prefix" && (uValue < 1 || uValue > 32)) ||
            (sName == "v6prefix" && (uValue < 1 || uValue > 128))) {
            PutModule(t_s("Prefix length out of range"));
            return;
        }

        SetNV(SET_PREFIX + sName, CString(uValue));
        PutModule(t_f("{1} = {2}")(sName, CString(uValue)));
    }

    void OnSettingsCommand(const CString& sLine) {
        CTable Table;
        Table.AddColumn(t_s("Setting"));
        Table.AddColumn(t_s("Value"));
        Table.AddColumn(t_s("Description"));
        for (const auto& it : Settings()) {
            Table.AddRow();
            Table.SetCell(t_s("Setting"), it.first);
            Table.SetCell(t_s("Value"), GetSetting(it.first));
            Table.SetCell(t_s("Description"), it.second.second);
        }

        PutModule(Table);
    }
};

template <>
void TModInfo<CIPGuardMod>(CModInfo& Info) {
    Info.SetHasArgs(false);
}

GLOBALMODULEDEFS(CIPGuardMod,
                 t_s("Block accounts that connect from the same IP prefix."))
