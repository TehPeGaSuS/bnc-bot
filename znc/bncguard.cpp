/*
 * SPDX-FileCopyrightText: 2026-present TehPeGaSuS
 *
 * SPDX-License-Identifier: MIT
 *
 * bncguard - account protection for a BNC service, in one global module.
 *
 * Shared IPs: on every login the client's IP is compared with the IPs of
 * every other account: the ones connected right now, and each account's
 * previous IPs seen within the last "window" hours (24 by default). When a
 * different account matches on the same IPv4 address / IPv6 prefix (/32 and
 * /64 by default), both accounts are blocked.
 *
 * Inactivity: the module records each account's last login/logout itself
 * (importing the lastseen module's times on first use). With "inactivedays"
 * set, an hourly check blocks accounts not seen for that many days. Accounts
 * with a client connected right now are active, whatever their last login.
 *
 * Blocks (shared IP, inactivity or manual "Block") disconnect the account's
 * clients and IRC networks and refuse further logins until "Unblock", which
 * also reconnects the networks that were connecting. The blockuser module's
 * list is imported on first use, so it can be unloaded afterwards.
 *
 * Admin users, exempt users and (for shared IPs) exempt CIDRs are never
 * checked. Automatic blocks are announced to all admin users from
 * *bncguard.
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
const char* const LASTSEEN_PREFIX = "lastseen:";
const char* const EXEMPT_PREFIX = "exempt:";
const char* const SET_PREFIX = "set:";
const char* const IMPORTED_KEY = "imported";

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

class CBNCGuardMod : public CModule {
  public:
    MODCONSTRUCTOR(CBNCGuardMod) {
        AddHelpCommand();
        AddCommand("Show", t_d("<user>"),
                   t_d("Show a user's block status, last seen time and IPs"),
                   [=](const CString& sLine) { OnShowCommand(sLine); });
        AddCommand("Users", "",
                   t_d("List every user's bindhost, last seen time and "
                       "block status"),
                   [=](const CString& sLine) { OnUsersCommand(sLine); });
        AddCommand("Shared", t_d("<user>"),
                   t_d("List accounts that shared an IP prefix with a user"),
                   [=](const CString& sLine) { OnSharedCommand(sLine); });
        AddCommand("List", "", t_d("List blocked users"),
                   [=](const CString& sLine) { OnListCommand(sLine); });
        AddCommand("Block", t_d("<user> [reason]"), t_d("Block a user"),
                   [=](const CString& sLine) { OnBlockCommand(sLine); });
        AddCommand("Unblock", t_d("<user>"),
                   t_d("Unblock a user and reconnect their networks"),
                   [=](const CString& sLine) { OnUnblockCommand(sLine); });
        AddCommand("Exempt", t_d("<user|cidr>"),
                   t_d("Never check a user (IP and inactivity) or network"),
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
        AddCommand("Import", "",
                   t_d("Import last seen times from lastseen and blocks from "
                       "blockuser again"),
                   [=](const CString& sLine) {
                       DelNV(IMPORTED_KEY);
                       Import();
                       if (GetNV(IMPORTED_KEY).empty()) {
                           PutModule(t_s("Neither lastseen nor blockuser is "
                                         "loaded"));
                       }
                   });
    }

    bool OnLoad(const CString& sArgs, CString& sMessage) override {
        AddTimer(new CInactivityTimer(this));
        // Loaded at runtime: the other modules are already there. At ZNC
        // startup they may load after us, so OnBoot tries again.
        Import();
        return true;
    }

    bool OnBoot() override {
        Import();
        return true;
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
        time_t now = time(nullptr);
        SetLastSeen(pUser->GetUsername(), now);

        Address addr;
        if (!ParseIP(sIP, addr)) return;

        if (!IsExemptUser(pUser) && !IsExemptIP(addr)) {
            CheckShared(pUser, sIP, addr, now);
        }

        RecordSeen(pUser->GetUsername(), sIP, now);
    }

    void OnClientDisconnect() override {
        // Refresh the times so the window and inactivity count from the
        // disconnect rather than the login
        if (GetUser() && GetClient()) {
            time_t now = time(nullptr);
            SetLastSeen(GetUser()->GetUsername(), now);
            RecordSeen(GetUser()->GetUsername(), GetClient()->GetRemoteIP(),
                       now);
        }
    }

    EModRet OnDeleteUser(CUser& User) override {
        DelNV(BLOCK_PREFIX + User.GetUsername());
        DelNV(SEEN_PREFIX + User.GetUsername());
        DelNV(LASTSEEN_PREFIX + User.GetUsername());
        DelNV(EXEMPT_PREFIX + User.GetUsername());
        return CONTINUE;
    }

    void OnModCommand(const CString& sCommand) override {
        if (!GetUser()->IsAdmin()) {
            PutModule(t_s("Access denied"));
        } else {
            HandleCommand(sCommand);
        }
    }

    void CheckInactive() {
        unsigned int uDays = GetSetting("inactivedays").ToUInt();
        if (!uDays) return;

        time_t now = time(nullptr);
        for (const auto& it : CZNC::Get().GetUserMap()) {
            CUser* pUser = it.second;
            const CString& sUser = pUser->GetUsername();
            time_t tLast = GetLastSeen(sUser);
            // Never seen (no record) isn't the same as inactive
            if (!tLast || IsExemptUser(pUser) || IsBlocked(sUser) ||
                !pUser->GetAllClients().empty()) {
                continue;
            }

            time_t tDays = (now - tLast) / 86400;
            if (tDays < (time_t)uDays) continue;

            CString sReason = "inactive " + CString((long long)tDays) + " days";
            Block(sUser, sReason);
            NotifyAdmins(t_f("Blocked {1}: {2}")(sUser, sReason));
        }
    }

  private:
    class CInactivityTimer : public CTimer {
      public:
        explicit CInactivityTimer(CBNCGuardMod* pModule)
            : CTimer(pModule, 3600, 0, "inactivity",
                     "Block accounts inactive for inactivedays days") {}

      protected:
        void RunJob() override {
            static_cast<CBNCGuardMod*>(GetModule())->CheckInactive();
        }
    };

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

    // Copy what the lastseen and blockuser modules know, once (until the
    // Import command asks again). Tried on every load/boot until at least
    // one of them was found loaded.
    void Import() {
        if (!GetNV(IMPORTED_KEY).empty()) return;

        CModules& Mods = CZNC::Get().GetModules();
        CModule* pLastSeen = Mods.FindModule("lastseen");
        CModule* pBlockUser = Mods.FindModule("blockuser");
        if (!pLastSeen && !pBlockUser) return;

        unsigned int uSeen = 0, uBlocked = 0;
        if (pLastSeen) {
            for (const auto& it : CZNC::Get().GetUserMap()) {
                const CString& sUser = it.first;
                time_t tTime = (time_t)pLastSeen->GetNV(sUser).ToLongLong();
                if (tTime > 0 && GetLastSeen(sUser) < tTime) {
                    SetLastSeen(sUser, tTime);
                    uSeen++;
                }
            }
        }

        if (pBlockUser) {
            for (auto it = pBlockUser->BeginNV(); it != pBlockUser->EndNV();
                 ++it) {
                if (CZNC::Get().FindUser(it->first) &&
                    !IsBlocked(it->first)) {
                    // blockuser already disconnected their networks
                    SetNV(BLOCK_PREFIX + it->first,
                          CString((long long)time(nullptr)) +
                              " - imported from blockuser");
                    uBlocked++;
                }
            }
        }

        SetNV(IMPORTED_KEY, CString((long long)time(nullptr)));
        NotifyAdmins(t_f("Imported {1} last seen times from lastseen and {2} "
                         "blocked users from blockuser")(uSeen, uBlocked));
    }

    time_t GetLastSeen(const CString& sUser) {
        return (time_t)GetNV(LASTSEEN_PREFIX + sUser).ToLongLong();
    }

    void SetLastSeen(const CString& sUser, time_t tTime) {
        SetNV(LASTSEEN_PREFIX + sUser, CString((long long)tTime));
    }

    unsigned int PrefixBits(const Address& addr) {
        return addr.family == AF_INET ? GetSetting("v4prefix").ToUInt()
                                      : GetSetting("v6prefix").ToUInt();
    }

    CString PrefixString(Address addr) {
        unsigned int uBits = PrefixBits(addr);
        Mask(addr, uBits);
        return FormatAddress(addr) + "/" + CString(uBits);
    }

    void CheckShared(CUser* pUser, const CString& sIP, const Address& addr,
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

    // Stored as "<time> <networks to restore, comma separated or -> <reason>"
    void Block(const CString& sUser, const CString& sReason) {
        CUser* pUser = CZNC::Get().FindUser(sUser);
        if (!pUser || IsBlocked(pUser->GetUsername())) return;

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

        CString sNetworks =
            CString(",").Join(vsNetworks.begin(), vsNetworks.end());
        SetNV(BLOCK_PREFIX + pUser->GetUsername(),
              CString((long long)time(nullptr)) + " " +
                  (sNetworks.empty() ? "-" : sNetworks) + " " + sReason);
    }

    bool IsBlocked(const CString& sUser) {
        return FindNV(BLOCK_PREFIX + sUser) != EndNV();
    }

    CString BlockSince(const CString& sValue) {
        return FormatTime((time_t)sValue.Token(0).ToLongLong());
    }

    CString BlockReason(const CString& sValue) {
        return sValue.Token(2, true);
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

    CString LastSeenText(CUser* pUser) {
        if (!pUser->GetAllClients().empty()) return t_s("connected now");
        time_t tLast = GetLastSeen(pUser->GetUsername());
        return tLast ? FormatTime(tLast) : t_s("never");
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
                {"inactivedays",
                 {"0", "Block accounts not seen for this many days, checked "
                       "hourly (0 = off)"}},
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

        const CString& sName = pUser->GetUsername();
        auto it = FindNV(BLOCK_PREFIX + sName);
        if (it != EndNV()) {
            PutModule(t_f("{1} is blocked since {2}: {3}")(
                sName, BlockSince(it->second), BlockReason(it->second)));
        }

        PutModule(t_f("{1} last seen: {2}")(sName, LastSeenText(pUser)));

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

        for (const Seen& seen : GetSeen(sName)) {
            Address addr;
            Table.AddRow();
            Table.SetCell(t_s("IP"), seen.sIP);
            if (ParseIP(seen.sIP, addr)) {
                Table.SetCell(t_s("Prefix"), PrefixString(addr));
            }
            Table.SetCell(t_s("Last seen"), FormatTime(seen.tTime));
        }

        if (!Table.empty()) PutModule(Table);
    }

    void OnUsersCommand(const CString& sLine) {
        CTable Table;
        Table.AddColumn(t_s("User"));
        Table.AddColumn(t_s("Bindhost"));
        Table.AddColumn(t_s("Last seen"));
        Table.AddColumn(t_s("Blocked"));
        for (const auto& it : CZNC::Get().GetUserMap()) {
            CUser* pUser = it.second;
            Table.AddRow();
            Table.SetCell(t_s("User"), pUser->GetUsername());
            Table.SetCell(t_s("Bindhost"), pUser->GetBindHost());
            Table.SetCell(t_s("Last seen"), LastSeenText(pUser));
            auto block = FindNV(BLOCK_PREFIX + pUser->GetUsername());
            if (block != EndNV()) {
                Table.SetCell(t_s("Blocked"), BlockReason(block->second));
            }
        }

        PutModule(Table);
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
            Table.SetCell(t_s("Since"), BlockSince(it->second));
            Table.SetCell(t_s("Reason"), BlockReason(it->second));
        }

        if (Table.empty()) {
            PutModule(t_s("No users are blocked"));
        } else {
            PutModule(Table);
        }
    }

    void OnBlockCommand(const CString& sLine) {
        CString sUser = sLine.Token(1);
        CString sReason = sLine.Token(2, true);
        if (sUser.empty()) {
            PutModule(t_s("Usage: Block <user> [reason]"));
            return;
        }

        CUser* pUser = FindUserOrComplain(sUser);
        if (!pUser) return;

        if (pUser == GetUser()) {
            PutModule(t_s("You can't block yourself"));
            return;
        }

        if (IsBlocked(pUser->GetUsername())) {
            PutModule(t_f("{1} is already blocked")(pUser->GetUsername()));
            return;
        }

        Block(pUser->GetUsername(),
              "manual (" + GetUser()->GetUsername() + ")" +
                  (sReason.empty() ? "" : ": " + sReason));
        PutModule(t_f("Blocked {1}")(pUser->GetUsername()));
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

            // Don't block them again for inactivity before they can log in
            SetLastSeen(pUser->GetUsername(), time(nullptr));
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
        if (sName == "inactivedays") CheckInactive();
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
void TModInfo<CBNCGuardMod>(CModInfo& Info) {
    Info.SetHasArgs(false);
}

GLOBALMODULEDEFS(CBNCGuardMod,
                 t_s("Shared IP, inactivity and manual blocks for a BNC "
                     "service."))
