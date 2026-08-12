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

## inspircd.conf Section-by-Section Explanation

All `&env.VARIABLE;` references resolve from the sourced `.env` files (`default.env` + `config.env`).

### `<include>` (lines 1–3)

```inspircd
<include file="/home/irc/modules.conf">
<include file="/home/irc/help.conf">
<include file="/home/irc/custom.conf">
```

Loads three external config files in order: `modules.conf` (module load list), `help.conf` (`/HELP` text), `custom.conf` (network-specific binds, TLS, link, connect classes, oper blocks).

---

### `<badip>` (lines 5–35)

Blocks connection attempts from IP ranges that should never be used by real users: APIPA (169.254), RFC 1918 private ranges, CGNAT (100.64), TEST-NET, multicast, DoD-assigned ranges, loopback, IPv4-mapped/compat, 6-to-4, ORCHIDv2, ULA, link-local.

---

### `<badnick>` (lines 37–53)

Reserves service nicknames so users cannot register them: ALIS, BOTSERV, CHANFIX, CHANSERV, GAMESERV, GLOBAL, GROUPSERV, HELPSERV, HOSTSERV, INFOSERV, MEMOSERV, NICKSERV, OPERSERV, PROXYSCAN, RPGSERV, SASLSERV, STATSERV.

---

### `<cidr>` (line 55)

Sets the CIDR length for clone detection. Two connections from the same `/32` IPv4 or `/64` IPv6 range are considered clones.

---

### `<class>` (lines 57–95)

Defines oper classes – permission groups assigned to oper types. Each class grants `commands`, `privs`, `snomasks`, `usermodes`, `chanmodes`.

| Class | Commands | Purpose |
|---|---|---|
| **Shutdown** | DIE, RESTART, REHASH, module load/unload | Full server control, all privs |
| **SACommands** | SAJOIN, SAPART, SANICK, SAQUIT, SATOPIC, SAKICK, SAMODE, OJOIN | Services-admin overrides |
| **ServerLink** | CONNECT, SQUIT, RCONNECT, RSQUIT, MKPASSWD, ALLTIME, SWHOIS, LOCKSERV, UNLOCKSERV | Server linking |
| **BanControl** | KILL, GLINE, KLINE, ZLINE, QLINE, ELINE, TLINE, RLINE, CHECK, NICKLOCK, NICKUNLOCK, SHUN, CLONES, CBAN | All ban types |
| **OperChat** | WALLOPS, GLOBOPS | Broadcast to opers |
| **HostCloak** | SETHOST, SETIDENT, SETIDLE, CHGNAME, CHGHOST, CHGIDENT | Change user metadata |
| **RolePlay** | (no commands, only priv) | Grants `channels/roleplay` and `channels/roleplay-override` |

---

### `<files>` (line 97)

MOTD and oper MOTD file paths (`/home/irc/motd.txt`, `/home/irc/oper.motd.txt`).

---

### `<insane>` (line 99)

Insane detection thresholds. When a single host/IP/nick matches more than `trigger` percent of users, oper notification fires.

---

### `<limits>` (lines 101–110)

Maximum lengths for away, chan, gecos, host, ident, kick, modes, nick, quit, topic. All from env vars.

---

### `<maxlist>` (line 112)

Limits `/LIST` results per channel to prevent flood.

---

### `<options>` (lines 114–136)

Core server options: default modes (`npsto`), host-in-topic, invite bypass, ping warning, prefix/suffix for part/quit, syntax hints, xline message, etc. All from env vars.

---

### `<path>` (lines 138–142)

Directories for data, config, runtime (PID), modules (`/usr/local/libexec/inspircd/modules/`), and logs.

---

### `<performance>` (lines 144–149)

Buffer sizes, clone-on-connect, quiet bursts, soft FD limit, listen backlog, time skip warning.

---

### `<pid>` (line 151)

PID file path (`/tmp/inspircd.pid`).

---

### `<security>` (lines 153–164)

Flat links, generic oper, hide bans/splits/ulines, custom version, max targets, restrict banned users, user stats visibility.

---

### `<type>` (lines 166–177)

Oper types that users authenticate as via `/OPER`:

| Type | Classes | Vhost |
|---|---|---|
| **NetAdmin** | SACommands + OperChat + BanControl + HostCloak + Shutdown + ServerLink | `admin/<network>` |
| **GlobalOp** | SACommands + OperChat + BanControl + HostCloak + ServerLink | `oper/<network>` |
| **Helper** | HostCloak only | `helper/<network>` |

Helper gets no ban/server commands, only metadata change.

---

### `<whowas>` (line 179)

WHOWAS history: group size, max groups, max keep time.

---

### `<maxmind>` (line 181)

GeoIP database path (`/home/irc/GeoLite2-Country.mmdb`) for country-based user classification.

---

### `<sts>` (lines 183–186)

IRCv3 Strict Transport Security – tells clients to only connect via TLS to `host:port` for the duration. Preload enabled for sharing in netbursts.

---

### `<sasl>` (lines 188–189)

SASL target server and whether TLS is required.

---

### `<alias>` (lines 191–304, 587)

Command aliases that transform user commands into `SQUERY` messages to services:

| User Text | Sends To | Purpose |
|---|---|---|
| `/ID <nick> <pass>` | ChanServ :IDENTIFY | Channel password auth |
| `/ID <nick> <pass>` | NickServ :IDENTIFY | Nick auth |
| `/NICKSERV <msg>` | NickServ | Full NS command access |
| `/CS <cmd>` | ChanServ | Channel command alias |
| `/BOTSERV` / `BS` | BotServ | BotServ access |
| `/CHANSERV` / `CS` | ChanServ | ChanServ access |
| `/HOSTSERV` / `HS` | HostServ | HostServ access |
| `/MEMOSERV` / `MS` | MemoServ | MemoServ access |
| `/NICKSERV` / `NS` | NickServ | NickServ access |
| `/OPERSERV` / `OS` | OperServ (oper-only) | OperServ access |
| `/STATSERV` / `SS` | StatServ | StatServ access |
| `/IDENTIFY <nick> <pass>` | NickServ | Explicit identify |
| `/GLOBAL <msg>` | GLOBAL (oper-only) | Global notice broadcast |
| `/HELPOP <topic>` | HELP | Help system |

---

### `<auditorium>` (lines 306–308)

Controls +u mode: whether ops/opers see the full user list and whether ops are visible.

---

### `<autodrop>` (line 310)

Silently drops HTTP commands (CONNECT, DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT, TRACE) at the IRC level.

---

### `<blockamsg>` (lines 312–313)

Anti-mass-message: after `delay` seconds of rapid multi-target messaging, `action` triggers (e.g. `killopers`).

---

### `<blockhighlight>` (lines 315–319)

Blocks excessive highlighting: min message length, min users involved, strip color, ignore ext messages.

---

### `<botmode>` (line 321)

Whether notices from +B (bot mode) users are forced through restrictions.

---

### `<callerid>` (lines 323–325)

Server-side ignore (+g): cooldown, max accepts, track by nick.

---

### `<cban>` (line 327)

Whether channel bans use glob matching.

---

### `<chanfilter>` (lines 329–331)

Channel content filter (+g): hide mask, max pattern length, notify user.

---

### `<chanhistory>` (lines 333–336)

Channel history (+H): bots receive history, opt-out via +N, max lines, prefix replay messages.

---

### `<chanlog>` (lines 338–339)

Sends snomask notifications into the oper channel (`#oper`).

---

### `<channames>` (lines 341–342)

Allowed/denied ASCII character ranges in channel names.

---

### `<channels>` (lines 344–345)

Max channels visible in WHOIS for opers and users (`4294967295` = unlimited).

---

### `<cloak>` (lines 347–349)

Host cloaking: `hmac-sha256-addr` method, secret key, suffix appended after hash. Used with +x mode.

---

### `<connectban>` (lines 351–357)

Anti-connection-hammering: threshold per CIDR, ban duration, boot wait, split wait, CIDR lengths.

---

### `<ctctags>` (line 359)

Whether client-only message tags are allowed.

---

### `<customprefix>` (lines 361–388)

Custom channel prefixes beyond standard `ov`:

| Prefix | Letter | Rank | Set by | Purpose |
|---|---|---|---|---|
| `~` founder | q | 50000 | Services only | Highest rank, cannot be kicked |
| `&` admin | a | 40000 | Services only | Admin level |
| `%` halfop | h | 20000 | +o can set | Limited op |
| `@` op | o | 30000 | +o can set | Standard op |
| `+` voice | v | 10000 | +o can set | Can speak in +m |

---

### `<deaf>` (lines 390–393)

Deaf (+d) and privdeaf (+D): bypass characters, U-line bypass, enable private deaf.

---

### `<delaymsg>` (line 395)

Whether notices bypass the +d (delay message) delay.

---

### `<disabled>` (lines 397–400)

Disabled commands, channel modes, user modes network-wide. `fakenonexistant` makes them appear missing.

---

### `<dnsbl>` (lines 402–427)

Three DNSBL checks on every connection:

| Name | Domain | Records | Action |
|---|---|---|---|
| **DroneBL** | `dnsbl.dronebl.org` | 3,5,6,7,8,9,10,11,13,14,15,16,17,19 | Z-line 5m |
| **EFnet RBL** | `rbl.efnetrbl.org` | 1,2,3,4,5 | Z-line 5m |
| **tor exit** | `torexit.dan.me.uk` | 100 | Z-line 5m |

---

### `<exemptfromfilter>` (lines 429–461)

Exempts all service nicknames (ALIS, BOTSERV, CHANSERV, etc.) from the content filter.

---

### `<hidechans>` (line 463)

Whether +I (hide channels in WHOIS) affects opers too.

---

### `<hidelist>` (lines 465–469)

Hides list modes from users below rank: filter (+g) hidden below op, invex (+I) visible to all.

---

### `<hidemode>` (lines 471–472)

Hides ban entries from users below voice rank (10000).

---

### `<hostname>` (line 474)

Character map for hostnames sent to clients (alphanumeric + `.-_/`).

---

### `<httpd>` (line 476)

HTTP daemon timeout for stats pages.

---

### `<inviteexception>` (line 478)

Whether invite exceptions (+I) bypass the channel key (+k).

---

### `<ircv3>` (lines 480–482)

IRCV3 capabilities: account-notify, away-notify, extended-join.

---

### `<joinflood>` (lines 484–486)

Anti-join-flood: boot wait, duration, split wait (used by +j mode).

---

### `<knock>` (line 488)

Who receives `/KNOCK` notifications (`both` = target user + channel ops).

---

### `<messageflood>` (lines 490–492)

Per-second flood limits for notice, privmsg, tagmsg (used by +f mode).

---

### `<monitor>` (line 494)

Max monitored nicknames per user.

---

### `<muteban>` (line 496)

Whether users are notified when +m (muteban) blocks their message.

---

### `<nickdelay>` (lines 498–499)

Prevents reclaiming a disconnected nick for `delay` seconds. `hint` tells the user.

---

### `<nickflood>` (line 501)

Nick flood detection duration (used by +F mode).

---

### `<noctcp>` (line 503)

Whether users can set +C (block CTCP) as a user mode.

---

### `<ojoin>` (lines 505–507)

Oper join: notice sent to channel, auto-op, message prefix.

---

### `<operlog>` (line 509)

Maps oper commands to snomask notifications (`on` = oper actions logged).

---

### `<opermotd>` (lines 511–512)

Shows oper MOTD after successful `/OPER`.

---

### `<operprefix>` (line 514)

Prefix added to oper nick in messages (e.g. `*`).

---

### `<override>` (lines 516–518)

Oper override (+O): enable, noisy announcements, require channel key.

---

### `<penalty>` (lines 520–521)

Command penalty in milliseconds (`HELPOP` costs 60ms).

---

### `<permchanneldb>` (lines 523–525)

Persists permanent channel (+P) modes, bans, topics to a SQLite DB.

---

### `<remove>` (lines 527–528)

`/REMOVE`: protected rank (founder), support nokicks.

---

### `<repeat>` (lines 530–534)

Repeat message blocking (+E): backlog, distance, lines, time window, size.

---

### `<rline>` (lines 536–538)

Regex banning: engine (`pcre`), match on nick change, zline on match.

---

### `<rotatelog>` (line 540)

Log rotation period (`86400` = daily).

---

### `<securelist>` (lines 542–544)

`/LIST` requires a wait period; registered users are exempt.

---

### `<showwhois>` (lines 546–547)

WHOIS notifications (+W): oper-only, show from opers.

---

### `<shun>` (lines 549–554)

Shun (restricted mode): affect opers, allow connect, allow tags, cleaned commands, enabled commands (AWAY, OPER, PING, etc.), notify user.

---

### `<silence>` (lines 556–557)

SILENCE: exempt U-lines, max entries.

---

### `<sslinfo>` (line 559)

Whether `/SSLINFO` is oper-only.

---

### `<sslmodes>` (line 561)

Whether users can set +z (TLS-only PMs).

---

### `<stdregex>` (line 563)

Regex engine type (`ecmascript`).

---

### `<svshold>` (line 565)

Whether SVS hold (nick locking by services) is silent.

---

### `<timedbans>` (line 567)

Notify user when timed ban expires.

---

### `<uline>` (lines 569–570)

Declares the services server as a U-line. U-lines are exempt from many restrictions.

---

### `<waitpong>` (lines 572–573)

Kill on bad PONG reply, send notice on wait-pong events.

---

### `<watch>` (line 575)

Max watched nicknames per user.

---

### `<wsorigin>` (line 577)

Allowed WebSocket origins.

---

### `<xlinedb>` (lines 579–580)

Persists X-lines to SQLite every `saveperiod` seconds.

---

### `<zombie>` (lines 582–585)

Zombie user cleanup after splits: clean split, dirty split, max zombies, server zombie time.

---

## custom.conf Sections

| Tag | Purpose |
|---|---|
| **`<sslprofile>`** | TLS profile with CA, cert, key, DH params |
| **`<oper>`** | Admin oper block with password from env |
| **`<exception>`** | Exception lines for Tailscale, localhost, Tor ULA |
| **`<bind>`** | HAProxy hook, TLS clients (SSL), server link (SSL), plain clients, HTTP stats |
| **`<link>`** | LINK block to services daemon (InspiServices) |
| **`<connect>`** | Connection classes: tor_haproxy_shim, tor, default, ssl, authenticated |
| **`<admin>`** | Admin contact |
| **`<server>`** | Server name, ID, network name |
| **`<operjoin>`** | Auto-join oper channel |
| **`<httpdacl>`** | HTTP ACL for stats page |
| **`<ident>`** | Ident service settings |
| **`<permchannels>`** | Permanent channel definitions (#oper, #services, #blackhole) |
| **`<exemptfromfilter>`** | Exempt oper and service channels from filters |
| **`<passforward>`** | Forward PASS to NickServ |

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

## Directory Setup

All config files live under `/home/irc/`. Deploy with:

```sh
cp -r examples/inspircd /home/irc/
ln -s /home/irc /etc/inspircd          # Debian/Ubuntu
# OR
ln -s /home/irc /usr/local/etc/inspircd  # FreeBSD/manual
chown -R irc:ircd /home/irc
```

For bash users, source the env files in `~/.bashrc` instead of `.tcshrc`:

```bash
source /home/irc/default.env
source /home/irc/config.env
```

Troubleshoot with debug foreground mode:

```sh
inspircd -d -F
```

## inspiservices Multi-Network Database Design

Different IRC networks can share the same inspircd + inspiservices stack while maintaining ownership of their users and data. This is achieved through:

- **Multimaster replication** across all services databases
- Every table row includes a **NETWORK column** indicating which network created it
- If a network decides to leave the partnership, it deletes all rows tagged with its NETWORK
- Remaining networks keep their data intact

This also ties back to the SID limit — each partner network consumes SIDs for its hub, services, and bridged virtual links. The 12,960 ceiling is shared across all co-operating networks.

## Bridge Virtual Link SID Limitation

Each bridge (Discord, Signal, etc.) is a virtual link — each bridged entity (a Discord channel within a guild, a Signal group) appears as a separate LINK to services. Each virtual link requires a unique SID (Server ID). SIDs are 3 bytes — the format is `nnn` or `nnA` where `n` is a decimal digit and `A` is an uppercase letter. This gives a total of 12,960 possible SIDs per server (`10 × 10 × 10 + 10 × 10 × 26 = 1000 + 2600 = 3600` per server, and with 3 servers per hub config that's `3600 × 3 = 10,800`, or up to `12,960` depending on allocation scheme).

**Practical limit: ~12,960 virtual links** across the hub/leaf set. If a bridged service needs more links than available SIDs, the SID storage type must be expanded — which has downstream implications for `LINKS`/`MAP` processing (sendq/recvq queues, netsplit handling, burst sizing).

This is an **experimental** bridging model. The 12,960 limit is generous for most deployments but should be investigated before scaling beyond that threshold.

### Production Example

A `/MAP` view showing hub.netcrave.chat linked to two other networks, each with their own services and bridges:

```
hub.netcrave.chat (01A InspIRCd-4.11.0-FreeBSD-4.11.0)
  ├─services.netcrave.chat (01B unknown)
  │   ├─foochat.123457.discord.netcrave.chat (01C)   (Discord #foochat in guild 123457)
  │   ├─barfoo.43215.discord.netcrave.chat   (01D)   (Discord #barfoo in guild 43215)
  │   └─foochat.12345.signal.netcrave.chat   (01E)   (Signal #foochat for user 12345)
  │
  ├─hub.supernets.org (11A InspIRCd-4.11.0-FreeBSD-4.11.0)
  │   └─services.supernets.org (11B unknown)
  │       ├─lobby.98765.discord.supernets.org (11C)  (Discord #lobby in guild 98765)
  │       └─dev.12345.discord.supernets.org   (11D)  (Discord #dev in guild 12345)
  │
  └─hub.ircfiber.com (21A InspIRCd-4.11.0-FreeBSD-4.11.0)
      └─services.ircfiber.com (21B unknown)
          ├─general.54321.discord.ircfiber.com (21C) (Discord #general in guild 54321)
          └─random.54321.discord.ircfiber.com   (21D) (Discord #random in guild 54321)
```

Naming conventions:
- Discord: `<channel>.<guild>.discord.<network>`
- Signal:  `<channel>.<userid>.signal.<network>`

Each server gets a unique SID — hub `01A`, services `01B`, bridged links `01C`–`01E` within netcrave, and similarly `11A`–`11D` for supernets and `21A`–`21D` for ircfiber.

## SSL_PORT / STS_PORT

`config.env` and `custom.conf` use `4443` for both ports. Change to `6697` for standard IRC TLS port if your environment allows it.

## Running

1. Source the environment:
   ```sh
   source /home/irc/default.env && source /home/irc/config.env
   ```
2. Start InspIRCd:
   ```sh
   inspircd --configdir /home/irc
   ```
3. The daemon reads `inspircd.conf` → includes `modules.conf`, `help.conf`, `custom.conf`.
4. Services (InspiServices) connect via the `<link>` block in `custom.conf`.
