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
- `lastseen_registry_path`: path to ZNC's `lastseen` module registry file (e.g.
  `/home/bnc/.znc/moddata/lastseen/.registry`), read directly from disk rather than queried over IRC to avoid
  flooding on BNC setups with many users. Enables the `lastseen <user>` command; leave empty to disable it.
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
- `max_inactive_days` / `inactive_check_interval_hours`: if `max_inactive_days` is non-zero, the bot periodically
  (every `inactive_check_interval_hours`, default `24`) checks each BNC user's last-seen time (requires
  `lastseen_registry_path`) and, if inactive longer than `max_inactive_days`, blocks them via ZNC's `blockuser`
  module — severing their current connections and refusing further logins, while leaving the account itself
  intact. Requires the `blockuser` module to be loaded globally. Re-enable with `unblockbnc` (or `Unblock <user>`
  via `blockuser` directly). `0` disables the check.
- `block_shared_ip`: if `true`, the bot blocks (via `blockuser`) every BNC account that attaches from the same IP
  prefix as a different account. Client IPs come from ZNC's `notify_connect` global module, which must be loaded
  (`/msg *status LoadMod --type=global notify_connect`) and broadcasts attaches/detaches to admin users; the bot's
  ZNC user must therefore be an admin and use ZNC's default (English) language. Both accounts are blocked and the
  event is logged to `log_channel`; use `unblockbnc` to lift a false positive (and exempt the account so it isn't
  blocked again). Defaults to `false`.
  - `shared_ip_v4_prefix` / `shared_ip_v6_prefix`: prefix length treated as "the same IP" (default `32` and `64`).
    IPv4-mapped IPv6 addresses (`::ffff:192.0.2.1`) are compared as IPv4.
  - `shared_ip_window_hours`: `0` (default) only matches accounts attached at the same time; a non-zero value also
    matches accounts that attached or detached from the prefix within that many hours. This history is kept in
    memory and starts empty whenever the bot restarts.
  - `shared_ip_exempt_users`: usernames never checked (the bot's own user is always exempt).
  - `shared_ip_exempt_nets`: CIDRs never checked, e.g. a webchat gateway, IRCCloud, or the ZNC host itself if
    users connect through a local web client.
- `blockuser_registry_path`: path to the `blockuser` module's registry file (e.g.
  `/home/bnc/.znc/moddata/blockuser/.registry`), read directly from disk to list currently blocked users. Enables
  the `listblocked` command; leave empty to disable it.
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

## Features
- Assigns each user a unique bindhost in the 127.0.0.0/16 range
- Generates a temporary password for a user on request approval and sends it to them through MemoServ
- Tracks existing BNC user accounts to avoid overwriting existing accounts
- Optionally blocks accounts that connect from the same IPv4 address or IPv6 /64 (`block_shared_ip`)

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

#### `denybnc <username>`
Deny a BNC account request for [username]

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

#### `lastseen <username>`
Show when [username] was last seen connected to the BNC (requires `lastseen_registry_path` to be configured)

#### `listusers`
Upload the full list of BNC users and their bindhosts to a pastebin and reply with the URL (requires
`pastebin_url` to be configured). Uploading avoids flooding the channel/PM on BNCs with many users.

#### `blockbnc <username>`
Block [username] via the `blockuser` module

#### `unblockbnc <username>`
Unblock [username] via the `blockuser` module

#### `listblocked`
List BNC users blocked via the `blockuser` module (requires `blockuser_registry_path` to be configured). Replies
inline for 2 or fewer users, otherwise uploads to a pastebin like `listusers`.

## Credits
This project is forked from [TotallyNotRobots/bnc-bot](https://github.com/TotallyNotRobots/bnc-bot).
