# InspIRCd Server Configuration (netcrave.chat)

This directory contains a complete InspIRCd configuration for a hub/leaf IRC network with services, TLS, Tor, HAProxy, and WebSocket support. Designed for the netcrave network, adaptable to any IRC network.

## File Overview

| File | Purpose |
|---|---|
| `inspircd.conf` | Main config – includes `modules.conf`, `help.conf`, `custom.conf` |
| `custom.conf` | Network-specific: TLS profiles, opers, binds, link block, connect classes |
| `modules.conf` | List of all loaded modules (198 modules) |
| `help.conf` | `/HELP` text for all user and oper commands, modes, extbans |
| `default.env` | Reference env template with all 214 variables set to defaults |
| `config.env` | Production env overrides for netcrave.chat |
| `.tcshrc` | Sources `default.env` then `config.env` for tcsh shells |
| `motd.txt` / `oper.motd.txt` | Message of the day files (empty by default) |
| `codepages/rfc1459.conf` | RFC 1459 character mapping + case table |
| `ca.crt` / `ca.key` | CA certificate (self-signed); **key file is placeholder** |
| `server.crt` / `server.key` | Server TLS certificate; **key file is placeholder** |
| `dh.pem` | Diffie-Hellman parameters; **placeholder** |

## Configuration Structure

Configuration uses `setenv` in `.env` files, referenced in config tags as `&env.VARIABLE;`. The `.tcshrc` sources the env files, then `inspircd` is launched with `--env` or the environment is exported before running.

### Sections in `inspircd.conf`

- **`<badip>`** – Block non-routable/reserved IP ranges (RFC 1918, APIPA, DoD, etc.)
- **`<badnick>`** – Reserve service nicknames so users cannot register them
- **`<cidr>`** – Clone detection CIDR limits
- **`<class>`** – Oper classes: Shutdown, SACommands, ServerLink, BanControl, OperChat, HostCloak, RolePlay
- **`<files>`** – MOTD and oper MOTD paths
- **`<insane>`** – Insane detection thresholds
- **`<limits>`** – Max lengths for away, chan, gecos, host, ident, kick, modes, nick, quit, topic
- **`<options>`** – Core options (default modes, host-in-topic, ping warning, etc.)
- **`<path>`** – Data/config/module/log directories
- **`<performance>`** – Buffers, clone limits, quiet bursts
- **`<pid>`** – PID file location
- **`<security>`** – Flat links, generic oper, hide bans/splits/ulines, custom version
- **`<type>`** – Oper types: NetAdmin, GlobalOp, Helper (with vhosts)
- **`<whowas>`** – WHOWAS history limits
- **`<maxmind>`** – GeoIP database path
- **`<sts>`** – IRCv3 STS policy (preload, host, port)
- **`<sasl>`** – SASL target server
- **`<alias>`** – Command aliases (ID, NS, CS, OS, BS, HS, MS, SS, GLOBAL, NICKSERV, etc.)
- **`<auditorium>`** – Auditorium mode settings
- **`<autodrop>`** – Drop HTTP commands at the IRC level
- **`<blockamsg>`** – Anti-mass-message protection
- **`<blockhighlight>`** – Highlight blocking
- **`<botmode>`** – Bot mode notice forwarding
- **`<callerid>`** – CallerID (server-side ignore) settings
- **`<cban>`** – Channel ban glob
- **`<chanfilter>`** – Channel content filter
- **`<chanhistory>`** – Channel history (H mode)
- **`<chanlog>`** – Log channel snomask
- **`<channames>`** – Allowed/denied channel name characters
- **`<channels>`** – Channel count limits
- **`<cloak>`** – Host cloaking (HMAC-SHA256-addr)
- **`<connectban>`** – Connection hammering protection
- **`<ctctags>`** – Client-only tag handling
- **`<customprefix>`** – Custom prefixes: founder (~), admin (&), halfop (%), op, voice
- **`<deaf>`** – Deaf mode bypass
- **`<delaymsg>`** – Delay message (d mode)
- **`<disabled>`** – Disabled commands/modes
- **`<dnsbl>`** – DNSBL checks (DroneBL, EFnet RBL, tor exit nodes)
- **`<exemptfromfilter>`** – Exempt service nicks from filters
- **`<hidechans>` / `<hidelist>` / `<hidemode>`** – Hidden channel/list/mode settings
- **`<hostname>`** – Hostname character map
- **`<httpd>`** – HTTP server timeout
- **`<inviteexception>`** – Invite exception bypass
- **`<ircv3>`** – IRCv3 capabilities (account-notify, away-notify, extended-join)
- **`<joinflood>`** – Join flood protection
- **`<knock>`** – Knock notification
- **`<messageflood>`** – Per-message flood thresholds
- **`<monitor>`** – MONITOR max entries
- **`<muteban>`** – Mute ban notifications
- **`<nickdelay>`** – Nick delay after disconnect
- **`<nickflood>`** – Nick flood duration
- **`<noctcp>`** – CTCP blocking
- **`<ojoin>`** – Oper join messages
- **`<operlog>`** – Oper log snomask
- **`<opermotd>`** – Oper MOTD
- **`<operprefix>`** – Oper prefix
- **`<override>`** – Oper override settings
- **`<penalty>`** – Command penalties
- **`<permchanneldb>`** – Permanent channel database
- **`<remove>`** – Remove command protection rank
- **`<repeat>`** – Repeat message blocking (E mode)
- **`<rline>`** – Regex-based line banning
- **`<rotatelog>`** – Log rotation period
- **`<securelist>`** – Secure LIST for registered users
- **`<showwhois>`** – WHOIS oper-only settings
- **`<shun>`** – Shun settings
- **`<silence>`** – SILENCE max entries
- **`<sslinfo>` / `<sslmodes>`** – SSL info and modes
- **`<stdregex>`** – Regex engine type
- **`<svshold>`** – SVS hold settings
- **`<timedbans>`** – Timed ban notifications
- **`<uline>`** – U-line server declaration
- **`<waitpong>`** – Wait-pong settings
- **`<watch>`** – WATCH max
- **`<wsorigin>`** – WebSocket origin allow
- **`<xlinedb>`** – X-line database persistence
- **`<zombie>`** – Zombie user cleanup
- **`<help>`** – HELP command alias

### Sections in `custom.conf`

- **`<sslprofile>`** – TLS profile with CA, cert, key, DH params
- **`<oper>`** – Admin oper block with password from env
- **`<exception>`** – Exception lines for Tailscale, localhost, Tor ULA
- **`<bind>`** – HAProxy hook, TLS clients (SSL), server link (SSL), plain clients, HTTP stats
- **`<link>`** – LINK block to services daemon (AnswerServices)
- **`<connect>`** – Connection classes: tor_haproxy_shim, tor, default, ssl, authenticated
- **`<admin>`** – Admin contact
- **`<server>`** – Server name, ID, network name
- **`<operjoin>`** – Auto-join oper channel
- **`<httpdacl>`** – HTTP ACL for stats page
- **`<ident>`** – Ident service settings
- **`<permchannels>`** – Permanent channel definitions (#oper, #services, #blackhole)
- **`<exemptfromfilter>`** – Exempt oper and service channels from filters
- **`<passforward>`** – Forward PASS to NickServ

## TLS Certificate Generation

The `ca.crt`, `server.crt`, `ca.key`, `server.key`, and `dh.pem` files are examples. **The key files are placeholders (`"need to generate your own"`).** Generate real keys:

```sh
# CA key + cert
openssl genrsa -out ca.key 2048
openssl req -new -x509 -key ca.key -out ca.crt -days 365 -subj "/CN=FakeCA"

# Server key + CSR + cert
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr -subj "/CN=FakeServer"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 365

# DH parameters
openssl dhparam -out dh.pem 2048
```

## Modules

`modules.conf` loads 198 modules covering:

- **Core** – channel, clients, DNS, hostname, info, list, message, mode, oper, user, wallops, who, whois, whowas, xline
- **Services integration** – `m_services`, `m_account`, `m_alias`, `m_passforward`, `m_sasl`
- **Security** – `m_connectban`, `m_dnsbl`, `m_filter`, `m_shun`, `m_rline`, `m_cban`, `m_classban`, `m_serverban`, `m_geoban`
- **Channel features** – `m_auditorium`, `m_chanhistory`, `m_chanfilter`, `m_hidelist`, `m_hidemode`, `m_permchannels`, `m_repeat`, `m_messageflood`
- **User features** – `m_callerid`, `m_silence`, `m_watch`, `m_monitor`, `m_botmode`, `m_deaf`, `m_delaymsg`, `m_ircv3*`
- **Networking** – `m_spanningtree` (server linking), `m_haproxy`, `m_websocket`, `m_httpd*`, `m_connectban`
- **Cloaking** – `m_cloak` (HMAC-SHA256), `m_cloak_sha256`, `m_cloak_md5`, `m_cloak_static`, `m_cloak_user`
- **Encoding** – `m_codepage` (rfc1459), `m_anticaps`, `m_blockcolor`, `m_stripcolor`, `m_channames`
- **Oper tools** – `m_operjoin`, `m_operlevels`, `m_operlog`, `m_opermodes`, `m_operprefix`, `m_override`, `m_ojoin`, `m_sajoin/sapart/samode/sanick/sakick/saquit/satopic`
- **Extbans** – `m_channelban`, `m_realnameban`, `m_nokicks`, `m_nonicks`, `m_nonotice`, `m_muteban`, `m_banredirect`, `m_allowinvite`
- **Regex** – `m_regex_glob`, `m_regex_posix`, `m_regex_stdlib`
- **SSL** – `m_ssl_openssl`, `m_sslinfo`, `m_sslmodes`, `m_starttls`

Some modules are commented out (not loaded): `m_log_sql`, `m_randquote`, `m_restrictchans`, `m_sqlauth`, `m_sqloper`.

## Environment Variables

There are 214 `setenv` variables across `default.env` and `config.env`. Key groups:

- **Server identity** – `SID`, `SERVER_NAME`, `NETWORK_NAME`, `CUSTOM_VERSION`
- **Ports** – `PORT`, `SSL_PORT`, `SERVERS_SSL_PORT`, `SERVER_SSL_PORT`, `HAPROXY_PORT`
- **Limits** – `MAX_AWAY`, `MAX_CHAN`, `MAX_NICK`, `MAX_TARGETS`, `LIST_MAX_SIZE`, etc.
- **Performance** – `NET_BUFFER_SIZE`, `SOFT_LIMIT`, `SO_MAX_CONN`, `QUIET_BURSTS`
- **Security** – `HIDE_SPLITS`, `HIDE_ULINES`, `FLAT_LINKS`, `GENERIC_OPER`, `CUSTOM_VERSION`
- **DNSBL** – Enabled via `m_dnsbl` with DroneBL, EFnet RBL, tor exit nodes
- **Cloaking** – `CLOAK_KEY`, `CLOAK_SUFFIX` (HMAC-SHA256-addr)
- **Link** – `SERVICES_ULINE`, `LINK_RECV_PASSWORD`, `LINK_SEND_PASSWORD`, `LINK_TIMEOUT`
- **Oper** – `ADMIN_PASSWORD`, `NET_ADMIN_VHOST`, `GLOBAL_OP_VHOST`, `HELPER_VHOST`
- **Services** – `SASL_TARGET`, `SERVICES_HOST`, `OPER_CHANNEL`, `SERVICE_CHANNEL`, `HELP_CHANNEL`
- **STS** – `STS_HOST`, `STS_PORT`, `STS_DURATION`
- **Channel features** – `CHAN_HISTORY_*`, `BLOCK_HL_*`, `REPEAT_*`, `JOIN_FLOOD_*`

## Running

1. Source the env (or use `--env` in newer InspIRCd builds):
   ```sh
   source .tcshrc   # or manually: source default.env && source config.env
   ```
2. Start InspIRCd:
   ```sh
   inspircd --configdir /home/irc
   ```
3. The daemon reads `inspircd.conf` → includes `modules.conf`, `help.conf`, `custom.conf`.
4. Services (AnswerServices) connect via the `<link>` block in `custom.conf`.
