# bnc-bot
A bot to handle BNC account requests for IRC networks

## Designed for
- [Anope](https://anope.org) IRC Services v2.0.15+ (tested up to v2.1.x)
- [ZNC](https://znc.in) v1.8+ (tested up to v1.11.x)
- [InspIRCd](https://inspircd.org) with the `m_services_account` module
- [UnrealIRCd](https://www.unrealircd.org)

This bot may work with other systems, but this is the setup it was specifically written to work with

## Requirements
- Python 3.10+
- A ZNC user named `BNCClient` to serve as the template for new accounts (see [Setup](#setup) below)
- The bot's own ZNC user must be an admin using ZNC's default (English) language, since the bot reads
  `controlpanel`'s replies when creating accounts
- The bundled `bncguard` ZNC module loaded globally (see [bncguard ZNC module](#bncguard-znc-module)) for blocks,
  last seen times and the inactivity lockout. The bot only talks to ZNC over its IRC connection, so it doesn't
  need to run on the ZNC host

## Setup
New BNC accounts are created by cloning a ZNC user named `BNCClient` (via `cloneuser BNCClient <username>`), then
overriding the password, bindhost, nick, altnick, ident, and realname for the new user. This `BNCClient` user must
exist on the ZNC server beforehand, and any other settings on it (attached networks, channels, modules, buffer
sizes, etc.) will be inherited by every new account created by the bot.

## Configuration Notes
- `bnc_network`: name of the ZNC network the bot reconnects new users to after account creation. This must match a
  network already attached to the ZNC template used for new accounts.
- `client_ssl_port` / `client_non_ssl_port`: SSL and non-SSL ports advertised to users in the BNC connection info
  memo sent on account creation/password reset. These are independent of `port`, which is only used for the bot's
  own admin connection to ZNC.
- `pastebin_url` / `pastebin_field`: upload endpoint and form field name for the `listusers` command. Must accept
  a `multipart/form-data` POST and respond with the plaintext paste URL. Defaults to `https://x0.at`.
  `pastebin_field` defaults to `file`,
  matching 0x0.st/x0.at and most compatible self-hosted clones (e.g.
  [single_php_filehost](https://github.com/Rouji/single_php_filehost), a lightweight self-hosted x0.at-compatible
  option). For [pinnwand](https://supakeen.com/project/pinnwand/)'s `/curl` endpoint, set `pastebin_field` to
  `raw` instead.
- `min_registered_days`: minimum age (in days) a services account must have before it may request a BNC, based on
  the registration date returned by NickServ's `INFO`. `0` disables the check; `config.default.json` ships with
  `7`.
- `max_inline_lines`: longest reply `deniedbnc` and the `bncguard` commands (`listblocked`, `ipshared`, ...) give
  inline before uploading it to the
  pastebin instead. Defaults to `3`.
- `channels`: channels the bot itself joins on connect (e.g. `log_channel`, since the bot only sends messages
  there but never joins it automatically otherwise).
- `user_channels`: channels automatically added to a new BNC user's `bnc_network` on account creation (via
  `controlpanel`'s `AddChan`), so ZNC joins them for the user on connect.

## IPv6 bindhosts
By default, bindhosts are assigned in the 127.0.0.0/16 IPv4 loopback range, which works out of the box on any
system. If you'd rather assign bindhosts from an IPv6 subnet, the kernel needs `net.ipv6.ip_nonlocal_bind` enabled
and a local route for that subnet on `lo`, otherwise ZNC can't bind to addresses that aren't actually configured on
an interface.

Tested on Ubuntu 24.04+ and should work unmodified on other Debian-based systems (systemd + `sysctl.d`). Other
distros may need adjusted paths/commands.

**Choosing a host.** Rather than recommending providers (that changes too often), look for three things:

- the provider routes a whole IPv6 subnet to your server, which the test below checks;
- its acceptable use policy allows IRC bouncers. Some hosts forbid them, or treat them as abuse once someone
  complains, however well the rest of the setup works;
- how the provider handles DDoS attacks against you. Bouncers attract them. Some providers filter attacks as part
  of the service; others cut your server off until it stops, and may close the account if an attack lasts or
  affects other customers, even when their acceptable use policy doesn't mention bouncers. Find out before your
  users depend on the server.

**Check your provider first.** This only works if your hosting provider routes the whole subnet to your machine.
Some cloud providers instead require every address to be assigned to the network interface through their own panel
or API, and silently drop traffic from any other address in the subnet. There, binding to the address succeeds but
the connection never gets out, whatever you configure on the machine. Which providers do this varies and changes
over time, so take any list you find with a grain of salt and test your own machine before setting anything up.
As root, with an address from your subnet that isn't configured on any interface:

```
ip route add local 2001:db8:0:0::/96 dev lo
sysctl -w net.ipv6.ip_nonlocal_bind=1
curl -6 --max-time 10 --interface 2001:db8::dead:beef https://ifconfig.co
```

If the address you picked comes back, the subnet is routed to you; continue with the steps below. Either way, undo
the test first with `ip route del local 2001:db8:0:0::/96 dev lo` and `sysctl -w net.ipv6.ip_nonlocal_bind=0` (the
steps below make both permanent).

If it times out, the subnet isn't routed to you and the steps below won't work, but IPv6 bindhosts still can: add
each one to the network interface yourself (in netplan or whatever manages your network, and in your provider's
panel if it requires that). Set `bind_host_net` as in step 4 and skip steps 1-3. The bot logs every new account's
bindhost to `log_channel` (`<user> was assigned bindhost <address>`), so you know which address to add; the account
can't connect to IRC until you have.

1. Create `/etc/systemd/system/net.ipv6.ip_nonlocal_bind.service` to add a local route for your subnet (replace
   `2001:db8:0:0::/96` with your own):

   ```
   [Unit]
   Description=Local routing table for IPv6
   After=network-online.target
   Wants=network-online.target

   [Service]
   Type=oneshot
   ExecStart=/sbin/ip route add local 2001:db8:0:0::/96 dev lo
   RemainAfterExit=yes

   [Install]
   WantedBy=multi-user.target
   ```

2. Create `/etc/sysctl.d/99-net.ipv6.ip_nonlocal_bind.conf` to enable non-local binding:

   ```
   net.ipv6.ip_nonlocal_bind = 1
   ```

3. Apply and enable both:

   ```
   sysctl --system
   systemctl enable --now net.ipv6.ip_nonlocal_bind.service
   ```

4. Change your `bind_host_net` to the same subnet:

   ```
   "bind_host_net": "2001:db8:0:0::/96",
   ```

## bncguard ZNC module
`znc/bncguard.cpp` is a global ZNC module that protects the BNC from inside ZNC, so the bot doesn't have to query
and parse anything for it:

- **Shared IPs**: on every login it compares the client's IP with the IPs of every other account: the ones connected
  right now, and each account's previous IPs seen within the last `window` hours. If a different account matches
  on the same IPv4 address or IPv6 /64, both accounts are blocked.
- **Inactivity**: it records every account's last login/logout itself and, with `inactivedays` set, checks hourly
  and blocks accounts not seen for that many days. Accounts with a client connected right now always count as
  active (ZNC's own `lastseen` only updates on login/logout, so an always-connected client would look inactive).
- **Blocks**: shared-IP, inactivity and manual blocks share one list with the reason for each. A block disconnects
  the account's clients and IRC networks and refuses further logins; `Unblock` reconnects exactly the networks
  that were connecting when it was blocked.

Admin users (including the bot) are never checked, and every automatic block is announced to admins from
`*bncguard`, which the bot posts to `log_channel`.

Build it like any other ZNC module, either by copying it into ZNC's `modules/` directory before running `make`, or
with `znc-buildmod znc/bncguard.cpp` (then copy `bncguard.so` to `~/.znc/modules/`). Load it globally:

```
/msg *status LoadMod --type=global bncguard
```

The first time it loads it imports the last seen times of the `lastseen` module and the blocked users of the
`blockuser` module if they're loaded, so both can be unloaded afterwards (`/msg *bncguard Import` repeats this).

It is configured through `/msg *bncguard <command>` (admins only); `Help` lists everything:

- `Set inactivedays <days>`: block accounts not seen for this many days (default `0`, off).
- `Set window <hours>`: how long an account's previous IPs still count (default `24`; `0` only compares accounts
  connected at the same time). Don't make this very long: ISPs reassign IPv4 addresses and CGNAT puts many
  customers behind one, so unrelated users can end up on each other's old addresses.
- `Set history <days>`: how long IP history is kept for `Show`/`Shared` (default `30`).
- `Set v4prefix <bits>` / `Set v6prefix <bits>`: prefix length treated as one IP (default `32` / `64`). IPv4-mapped
  IPv6 addresses (`::ffff:192.0.2.1`) count as IPv4.
- `Exempt <user|cidr>` / `Unexempt <user|cidr>` / `Exempts`: never check an account (IP and inactivity) or a
  network (IP), e.g. a webchat gateway or IRCCloud's ranges.
- `Show`, `Users`, `Shared`, `List`, `Block`, `Unblock`: what the bot's commands below relay to.

## Features
- Assigns each user a unique bindhost in the 127.0.0.0/16 range
- Generates a temporary password for a user on request approval and sends it to them through MemoServ
- Tracks existing BNC user accounts to avoid overwriting existing accounts
- Blocks accounts that share an IP or stay inactive too long via the bundled `bncguard` ZNC module

## Installation
1. Set up a Python 3 virtualenv
2. `pip install .`
3. Copy `config.default.json` to `config.json` and modify the values as needed
4. Run `bnc-bot` to start the bot
  _note: check `bnc-bot --help` for command line options_


## Commands
### User Commands
#### `requestbnc`
Submit a BNC account request

### Admin Commands
#### `acceptbnc <username>`
Accept a BNC account request for [username]

#### `denybnc <username> [reason]`
Deny a BNC account request for [username]. The denial is recorded with the time, the admin and the optional reason
(the reason is not sent to the user). If the account requests again, the log channel line notes the earlier
denials.

#### `deniedbnc [account]`
List denied BNC requests, newest first, optionally for one account only. Replies inline for up to
`max_inline_lines` lines, otherwise uploads the list to a pastebin like `listusers`.

#### `cleardenied <account>`
Forget an account's denied BNC requests.

#### `delbnc <username>`
Delete [username]'s BNC account

#### `bncresetpass <username>`
Reset [username]'s BNC account password

#### `bncqueue`
List all current entries in the BNC account request queue awaiting approval

#### `bncsetadmin <username>`
Grant [username] BNC admin access

#### `bncrefresh`
Update the cached version of the BNC user list

#### `lastseen <username>` (aliases `bncshow`, `bncip`, `ipshow`)
Show [username]'s block status, last seen time, currently connected IPs and recent IP history.

#### `listusers`
Upload every ZNC user with their bindhost, last seen time and block status to a pastebin and reply with the URL
(requires `pastebin_url` to be configured). Uploading avoids flooding the channel/PM on BNCs with many users.

#### `blockbnc <username> [reason]`
Block [username]: their clients and IRC networks are disconnected and further logins refused.

#### `unblockbnc <username>` (alias `ipunblock`)
Unblock [username], whatever the block's reason, and reconnect the networks that were connecting when they were
blocked.

#### `listblocked` (aliases `bncblocked`, `ipblocked`)
List blocked users with when and why (shared IP, inactivity, manual).

#### `ipshared <username>` (alias `bncshared`)
List every account that has shared an IP prefix with a user (within the IP history, ignoring exemptions).

These commands pass `*bncguard`'s reply on unchanged: inline up to `max_inline_lines` lines, otherwise via the
pastebin.

## Credits
This project is forked from [TotallyNotRobots/bnc-bot](https://github.com/TotallyNotRobots/bnc-bot).
