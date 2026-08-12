# bnc-bot
A bot to handle BNC account requests for IRC networks

## Designed for
- [Anope](https://anope.org) IRC Services v2.0.15
- [ZNC](https://znc.in) v1.8.2
- [InspIRCd](https://inspircd.org) with the `m_services_account` module

This bot may work with other systems, but this is the setup it was specifically written to work with

## Requirements
- Python 3.10+
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
  the registration date returned by NickServ's `INFO`. `0` (default) disables the check.
- `channels`: channels the bot itself joins on connect (e.g. `log_channel`, since the bot only sends messages
  there but never joins it automatically otherwise).
- `user_channels`: channels suggested to users in the credentials memo sent when their BNC account is created.
  Purely informational; the bot does not invite or join users to these itself.

## Features
- Assigns each user a unique bindhost in the 127.0.0.0/16 range
- Generates a temporary password for a user on request approval and sends it to them through MemoServ
- Tracks existing BNC user accounts to avoid overwriting existing accounts

## Installation
1. Set up a Python 3 virtualenv
2. `pip install .`
3. Copy `config.default.json` to `config.json` and modify the values as needed
4. Run `bnc-bot` to start the bot
  _note: check `bnc-bot --help` for command line options_

### Docker
An official docker image is published at `ghcr.io/totallynotrobots/bnc-bot` for this app.

To use it you can:
1. Create a `config.json` following the format mentioned above
2. Run the container with `docker run -v /path/to/config.json:/config/config.json ghcr.io/totallynotrobots/bnc-bot:latest`

An optional `/data` volume exists as well which is where logs and runtime state are stored. This can be attached to a host directory with `-v /path/to/data:/data`.

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

## Credits
This project is forked from [TotallyNotRobots/bnc-bot](https://github.com/TotallyNotRobots/bnc-bot).
