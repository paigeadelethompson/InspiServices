# inspircd.conf Section-by-Section Explanation

All `&env.VARIABLE;` references resolve from the sourced `.env` files (`default.env` + `config.env`). Every tag below is in `inspircd.conf` unless noted as coming from `custom.conf`.

## Directory Setup

All config files expect to live under `/home/irc/` (the home of the `irc` system user). To deploy:

```sh
# Copy the entire examples/inspircd/ directory to the target
cp -r examples/inspircd /home/irc/

# Symlink the system config directory to /home/irc
# (pick one based on your OS package convention)
ln -s /home/irc /etc/inspircd       # Debian/Ubuntu
# OR
ln -s /home/irc /usr/local/etc/inspircd  # FreeBSD / manual install

# Set ownership so InspIRCd can read/write
chown -R irc:ircd /home/irc
# (ircd is the group the inspircd package creates; the binary runs as this user)
```

### Loading the Environment

The `.tcshrc` file sources both env files for tcsh users. For bash users, use `.bashrc` instead:

```bash
# in ~/.bashrc
source /home/irc/default.env
source /home/irc/config.env
```

### Troubleshooting

Run InspIRCd in debug mode with foreground output to see all config parsing and module loading:

```sh
inspircd -d -F
```

This shows every config tag as it's parsed, any errors, and full log output to stderr.

## SSL_PORT / STS_PORT

In `config.env` and `custom.conf`, `SSL_PORT` and `STS_PORT` are set to `4443`. This was due to technical limitations with that port on a specific setup. **Change both to `6697`** for standard IRC-over-TLS port compliance if your environment allows it.

---

## `<include>` (lines 1–3)

```inspircd
<include file="/home/irc/modules.conf">
<include file="/home/irc/help.conf">
<include file="/home/irc/custom.conf">
```

Loads three external config files in order:
- **modules.conf** – the module load list
- **help.conf** – `/HELP` text definitions
- **custom.conf** – network-specific binds, TLS, link block, connect classes, oper blocks

These are treated as if their contents were pasted inline into `inspircd.conf`.

---

## `<badip>` (lines 5–35)

Blocks connection attempts from IP ranges that should never be used by real users. Each entry:
- `ipmask` – CIDR range to block
- `reason` – displayed to the user on connection rejection

Covers: APIPA (169.254), RFC 1918 private ranges, CGNAT (100.64), TEST-NET ranges, multicast, DoD-assigned ranges, loopback, IPv4-mapped/compat, 6-to-4, ORCHIDv2, ULA, link-local. These prevent misconfigured proxies, LAN traffic, and reserved-space abuse from reaching the IRCd.

---

## `<badnick>` (lines 37–53)

Reserves nicknames so users cannot register them. Each entry:
- `nick` – the reserved nickname (case-insensitive)
- `reason` – shown when a user tries to use it

Reserved: ALIS, BOTSERV, CHANFIX, CHANSERV, GAMESERV, GLOBAL, GROUPSERV, HELPSERV, HOSTSERV, INFOSERV, MEMOSERV, NICKSERV, OPERSERV, PROXYSCAN, RPGSERV, SASLSERV, STATSERV. These are all common service names.

---

## `<cidr>` (line 55)

```inspircd
<cidr ipv4clone="&env.IPV4_CLONE;" ipv6clone="&env.IPV6_CLONE;">
```

Sets the CIDR length used for clone detection. Two connections from the same `/32` IPv4 or `/64` IPv6 range are considered clones. Used by the `m_connectban` module and `/CLONES` command.

---

## `<class>` (lines 57–95)

Defines **oper classes** – permission groups assigned to oper types. Each class grants:
- `commands` – which oper commands the class can run
- `privs` – granular permission strings (e.g. `users/auspex` for user info, `channels/auspex` for channel info)
- `snomasks` – server notice masks the class can see
- `usermodes` – user modes the class can set
- `chanmodes` – channel modes the class can set

Classes defined here:

| Class | Commands | Purpose |
|---|---|---|
| **Shutdown** | DIE, RESTART, REHASH, module load/unload | Full server control, all privs |
| **SACommands** | SAJOIN, SAPART, SANICK, SAQUIT, SATOPIC, SAKICK, SAMODE, OJOIN | Services-admin level overrides |
| **ServerLink** | CONNECT, SQUIT, RCONNECT, RSQUIT, MKPASSWD, ALLTIME, SWHOIS, LOCKSERV, UNLOCKSERV | Server linking control |
| **BanControl** | KILL, GLINE, KLINE, ZLINE, QLINE, ELINE, TLINE, RLINE, CHECK, NICKLOCK, NICKUNLOCK, SHUN, CLONES, CBAN | All ban types |
| **OperChat** | WALLOPS, GLOBOPS | Broadcast messages to opers |
| **HostCloak** | SETHOST, SETIDENT, SETIDLE, CHGNAME, CHGHOST, CHGIDENT | Change user metadata |
| **RolePlay** | (no commands, only priv) | Grants `channels/roleplay` and `channels/roleplay-override` for RP channels |

---

## `<files>` (line 97)

```inspircd
<files motd="/home/irc/motd.txt" opermotd="/home/irc/oper.motd.txt">
```

Sets paths for the Message of the Day (shown to users on connect) and the oper MOTD (shown after `/OPER`).

---

## `<insane>` (line 99)

```inspircd
<insane hostmasks="&env.INSANE_HOSTMASKS;" ipmasks="&env.INSANE_IPMASKS;"
        nickmasks="&env.INSANE_NICKMASKS;" trigger="&env.INSANE_TRIGGER;">
```

Configures the **insane detection** system. When a single host/IP/nick matches more than `trigger` percent of the current user count, oper notification fires. Disabled by default (set to `no`).

---

## `<limits>` (lines 101–110)

Sets maximum lengths for various user input fields:
- `maxaway` – away message length
- `maxchan` – channels per user
- `maxgecos` – realname (GECOS) length
- `maxhost` – hostname length
- `maxident` – username length
- `maxkick` – kick message length
- `maxmodes` – modes per MODE command
- `maxnick` – nickname length
- `maxquit` – quit message length
- `maxtopic` – topic length

All pulled from env vars, tunable per network.

---

## `<maxlist>` (line 112)

```inspircd
<maxlist chan="*" limit="&env.LIST_MAX_SIZE;">
```

Limits the number of entries returned by `/LIST` to prevent flood. Applies to all channels.

---

## `<options>` (lines 114–136)

Core server options, most pulled from env:

- `allowmismatch` – allow nick registration with mismatched case
- `allowzerolimit` – allow `+l 0` (unlimited users)
- `announcets` – announce timestamp changes to users
- `cyclehosts` – cycle display host when cycling (joining after part)
- `cyclehostsfromuser` – whether cycle host comes from user's IP
- `defaultbind` – socket bind strategy (`auto` = try IPv6, fall back to IPv4)
- `defaultmodes` – modes applied to new channels (`npsto`)
- `exemptchanops` – which chanop levels bypass which restrictions (`censor:o filter:o nickflood:o nonick:v regmoderated:o`)
- `fixedpart` / `fixedquit` – override part/quit messages (empty = use user's message)
- `hostintopic` – show hostmask in topic
- `invitebypassmodes` – whether invite bypasses certain channel modes
- `modesinlist` – show modes in `/LIST`
- `nosnoticestack` – don't stack snomask notifications
- `pingwarning` – seconds before server warns of slow ping
- `prefixpart` / `prefixquit` – prefix for part/quit messages
- `serverpingfreq` – seconds between server-to-server pings
- `splitwhois` – split WHOIS replies into separate messages
- `suffixpart` / `suffixquit` – suffix for part/quit messages
- `syntaxhints` – show syntax hints on command errors
- `xlinemessage` – text sent as fake quit message on X-line reject

---

## `<path>` (lines 138–142)

```inspircd
<path datadir="/home/irc" configdir="/home/irc" runtimedir="/tmp"
      moduledir="/usr/local/libexec/inspircd/modules/" logdir="/home/irc">
```

Sets directory paths:
- `datadir` – where data files (DBs, MOTD, etc.) are stored
- `configdir` – where config files are read from
- `runtimedir` – for PID file and runtime sockets
- `moduledir` – where `.so` module files live
- `logdir` – where log files are written

---

## `<performance>` (lines 144–149)

```inspircd
<performance clonesonconnect="&env.CLONES_ON_CONNECT;"
             netbuffersize="&env.NET_BUFFER_SIZE;"
             quietbursts="&env.QUIET_BURSTS;"
             softlimit="&env.SOFT_LIMIT;"
             somaxconn="&env.SO_MAX_CONN;"
             timeskipwarn="&env.TIME_SKIP_WARN;">
```

Performance tuning:
- `clonesonconnect` – treat multiple simultaneous connects as clones
- `netbuffersize` – per-connection buffer size (bytes)
- `quietbursts` – suppress join/part messages during netbursts
- `softlimit` – max file descriptors before refusing connections
- `somaxconn` – listen backlog size
- `timeskipwarn` – warn when system time jumps by this much

---

## `<pid>` (line 151)

```inspircd
<pid file="/tmp/inspircd.pid">
```

Path for the PID file, used for shutdown signals and detecting running instances.

---

## `<security>` (lines 153–164)

```inspircd
<security allowcoreunload="&env.ALLOW_CORE_UNLOAD;"
          announceinvites="&env.ANNOUNCE_INVITES;"
          customversion="&env.CUSTOM_VERSION;"
          flatlinks="&env.FLAT_LINKS;"
          genericoper="&env.GENERIC_OPER;"
          hidebans="&env.HIDE_BANS;"
          hidemodes="&env.HIDE_MODES;"
          hidesplits="&env.HIDE_SPLITS;"
          hideulines="&env.HIDE_ULINES;"
          maxtargets="&env.MAX_TARGETS;"
          restrictbannedusers="&env.RESTRICT_BANNED_USERS;"
          userstats="&env.USER_STATS;">
```

Security-related settings:
- `allowcoreunload` – whether core modules can be unloaded (dangerous, disabled)
- `announceinvites` – whether invites are announced to chanops
- `customversion` – overrides the VERSION string shown to users
- `flatlinks` – hides server-to-server link topology
- `genericoper` – hides oper type from WHOIS
- `hidebans` – hides ban matches from users
- `hidemodes` – hides certain modes from non-opers
- `hidesplits` – hides split/join messages
- `hideulines` – hides U-line servers from `/MAP`/`/LINKS`
- `maxtargets` – max targets per command
- `restrictbannedusers` – prevent banned users from staying connected
- `userstats` – which stats are visible to users (vs oper-only)

---

## `<type>` (lines 166–177)

Defines **oper types** – the actual operator ranks that users authenticate as via `/OPER`. Each type:
- `classes` – which oper classes' permissions it inherits
- `modes` – user modes automatically set on oper-up (`+s +cCqQ`)
- `name` – oper type name (used in `/OPER <user> <pass>`)
- `vhost` – cloaked host shown when oper is oper'd up

| Type | Classes | Vhost |
|---|---|---|
| **NetAdmin** | SACommands + OperChat + BanControl + HostCloak + Shutdown + ServerLink | `admin/<network>` |
| **GlobalOp** | SACommands + OperChat + BanControl + HostCloak + ServerLink | `oper/<network>` |
| **Helper** | HostCloak only | `helper/<network>` |

Helper gets no ban/server control commands, only metadata change.

---

## `<whowas>` (line 179)

```inspircd
<whowas groupsize="&env.WHOWAS_GROUP_SIZE;" maxgroups="&env.WHOWAS_MAX_GROUPS;"
        maxkeep="&env.WHOWAS_MAX_KEEP;">
```

Controls the WHOWAS history system:
- `groupsize` – how many entries per group
- `maxgroups` – max groups before old ones are pruned
- `maxkeep` – maximum time to keep entries (32 years = effectively forever)

---

## `<maxmind>` (line 181)

```inspircd
<maxmind file="/home/irc/GeoLite2-Country.mmdb">
```

Path to the MaxMind GeoIP database for country-based user classification (used by `m_geoban` and `m_geoclass`).

---

## `<sts>` (lines 183–186)

```inspircd
<sts duration="5m" host="&env.STS_HOST;" port="&env.SSL_PORT;" preload="yes">
```

**IRCV3 Strict Transport Security** – tells clients to only connect via TLS to `host:port` for `duration`. `preload=yes` means servers can share STS policies in netbursts.

---

## `<sasl>` (lines 188–189)

```inspircd
<sasl requiressl="&env.SASL_REQUIRE_SSL;" target="&env.SASL_TARGET;">
```

SASL authentication target:
- `requiressl` – SASL only allowed over TLS
- `target` – the server name that handles SASL (the services daemon)

---

## `<alias>` (lines 191–304, 587)

Defines command aliases – shortcuts that transform user commands into `SQUERY` messages to services. Key fields:
- `text` – the alias command users type
- `replace` – what the server sends instead (usually `SQUERY <service> :<message>`)
- `requires` – the service nickname that must be online for the alias to work
- `uline` – whether the target is a U-line service
- `format` – pattern matching for arguments
- `operonly` – only opers can use this alias
- `channelcommand` – alias can be used in channel as a message

Aliases defined:

| User Text | Sends To | Purpose |
|---|---|---|
| `/ID <nick> <pass>` | ChanServ :IDENTIFY | Channel password auth |
| `/ID <nick> <pass>` | NickServ :IDENTIFY | Nick auth |
| `/NICKSERV <msg>` | NickServ | Full NS command access |
| `/CS <cmd>` | ChanServ | Channel command alias |
| `/BOTSERV <msg>` | BotServ | BotServ access |
| `/CHANSERV <msg>` | ChanServ | ChanServ access |
| `/HOSTSERV <msg>` | HostServ | HostServ access |
| `/MEMOSERV <msg>` | MemoServ | MemoServ access |
| `/NICKSERV <msg>` | NickServ | NickServ access |
| `/OPERSERV <msg>` | OperServ (oper-only) | OperServ access |
| `/STATSERV <msg>` | StatServ | StatServ access |
| `/BS <msg>` | BotServ | Short BotServ alias |
| `/CS <msg>` | ChanServ | Short ChanServ alias |
| `/HS <msg>` | HostServ | Short HostServ alias |
| `/MS <msg>` | MemoServ | Short MemoServ alias |
| `/NS <msg>` | NickServ | Short NickServ alias |
| `/OS <msg>` | OperServ (oper-only) | Short OperServ alias |
| `/SS <msg>` | StatServ | Short StatServ alias |
| `/IDENTIFY <nick> <pass>` | NickServ | Explicit identify |
| `/GLOBAL <msg>` | GLOBAL (oper-only) | Global notice broadcast |
| `/HELPOP <topic>` | HELP | Help system |

The final alias `text="HELPOP" replace="HELP $2-"` redirects `/HELPOP` to the help system.

---

## `<auditorium>` (lines 306–308)

```inspircd
<auditorium opcansee="&env.AUDITORIUM_OP_CAN_SEE;"
            opercansee="&env.AUDITORIUM_OPER_CAN_SEE;"
            opvisible="&env.AUDITORIUM_OP_VISIBLE;">
```

Controls the +u (auditorium) channel mode:
- `opcansee` – whether channel ops see the full user list
- `opercansee` – whether server opers see the full user list
- `opvisible` – whether ops are visible in the auditorium

---

## `<autodrop>` (line 310)

```inspircd
<autodrop commands="CONNECT DELETE GET HEAD OPTIONS PATCH POST PUT TRACE">
```

Silently drops HTTP commands at the IRC level, preventing clients from accidentally sending HTTP requests (common with misconfigured proxies).

---

## `<blockamsg>` (lines 312–313)

```inspircd
<blockamsg action="&env.BLOCK_AMSG_ACTION;" delay="&env.BLOCK_AMSG_DELAY;">
```

Anti-mass-message protection. When a user sends the same message to many targets rapidly, after `delay` seconds the `action` triggers (e.g. `killopers` = kill with oper notification).

---

## `<blockhighlight>` (lines 315–319)

```inspircd
<blockhighlight ignoreextmsg="&env.BLOCK_HL_IGNORE_EXT_MESSAGE;"
                minlen="&env.BLOCK_HL_MIN_LEN;"
                minusernum="&env.BLOCK_HL_MIN_USER_NUM;"
                reason="highlighting has been blocked (exceeded limits)"
                stripcolor="&env.BLOCK_HL_STRIP_COLOR;">
">

Blocks excessive highlighting (massive repeated ping/mention):
- `minlen` – minimum message length to check
- `minusernum` – minimum users involved before blocking
- `stripcolor` – strip formatting before checking
- `ignoreextmsg` – ignore extended messages

---

## `<botmode>` (line 321)

```inspircd
<botmode forcenotice="&env.BOT_MODE_FORCE_NOTICE;">
```

Controls how +B (bot mode) interacts with notices. When enabled, notices from bots are forced through regardless of other restrictions.

---

## `<callerid>` (lines 323–325)

```inspircd
<callerid cooldown="&env.CALLER_ID_COOL_DOWN;"
          maxaccepts="&env.CALLER_ID_MAX_ACCEPTS;"
          tracknick="&env.CALLER_ID_TRACK_NICK;">
">

Server-side ignore (+g mode):
- `cooldown` – how long before an accepted nick can be re-added
- `maxaccepts` – max accepted nicks per user
- `tracknick` – track accepted nicks by nick, not user@host

---

## `<cban>` (line 327)

```inspircd
<cban glob="&env.CBAN_GLOB;">
">

Whether channel bans (CBAN) use glob matching or exact matching.

---

## `<chanfilter>` (lines 329–331)

```inspircd
<chanfilter hidemask="&env.CHAN_FILTER_HIDE_MASK;"
            maxlen="&env.CHAN_FILTER_MAX_LEN;"
            notifyuser="&env.CHAN_FILTER_NOTIFY_USER;">
">

Controls +g (channel filter) mode:
- `hidemask` – hide the filter mask from non-opers
- `maxlen` – max length of a filter pattern
- `notifyuser` – notify the user when their message is filtered

---

## `<chanhistory>` (lines 333–336)

```inspircd
<chanhistory bots="&env.CHAN_HISTORY_BOTS;"
             enableumode="&env.CHAN_HISTORY_ENABLE_UMODE;"
             maxlines="&env.CHAN_HISTORY_MAX_LINES;"
             prefixmsg="&env.CHAN_HISTORY_PREFIX_MSG;">
">

Controls +H (channel history):
- `bots` – whether bots receive history
- `enableumode` – whether users can opt out via +N
- `maxlines` – max lines stored per channel
- `prefixmsg` – prefix history replay messages with a tag

---

## `<chanlog>` (lines 338–339)

```inspircd
<chanlog channel="&env.OPER_CHANNEL;" snomasks="&env.OPER_CHANNEL_SNOMASK;">
">

Sends snomask notifications into a channel (`#oper`) so opers can see server notices in the channel.

---

## `<channames>` (lines 341–342)

```inspircd
<channames allowrange="&env.CHAN_NAMES_ALLOW_RANGE;"
           denyrange="CHAN_NAMES_DENY_RANGE;">
">

Defines ASCII character ranges allowed and denied in channel names. This config allows `#`, `%`, `&`, and some punctuation while denying control characters, spaces, and DEL.

---

## `<channels>` (lines 344–345)

```inspircd
<channels opers="&env.CHANNELS_OPERS;" users="&env.CHANNELS_USERS;">
">

Sets the maximum number of channels visible in WHOIS for opers and users. `4294967295` = unlimited (max uint32).

---

## `<cloak>` (lines 347–349)

```inspircd
<cloak method="hmac-sha256-addr"
       key="&env.CLOAK_KEY;"
       suffix="&env.CLOAK_SUFFIX;">
">

Host cloaking configuration:
- `method` – `hmac-sha256-addr` (address-based HMAC)
- `key` – secret key for HMAC (must be unique per network)
- `suffix` – string appended after the cloaked host

Produces cloaked hosts like `user@<hash>.hidden` when user has +x mode.

---

## `<connectban>` (lines 351–357)

```inspircd
<connectban banmessage="filtered for connection hammering; wait 64 seconds to retry"
            bootwait="&env.CONNECT_BAN_BOOT_WAIT;"
            duration="&env.CONNECT_BAN_DURATION;"
            ipv4cidr="&env.CONNECT_BAN_V4_PREFIX_LEN;"
            ipv6cidr="&env.CONNECT_BAN_v6_PREFIX_LEN;"
            splitwait="&env.CONNECT_BAN_SPLIT_WAIT;"
            threshold="&env.CONNECT_BAN_THRESHOLD;">
">

Anti-connection-hammering:
- `threshold` – connects per CIDR before ban triggers
- `duration` – how long the ban lasts
- `bootwait` – delay after server boot before detection starts
- `splitwait` – delay after netburst before detection resumes
- `ipv4cidr` / `ipv6cidr` – CIDR length for tracking

---

## `<ctctags>` (line 359)

```inspircd
<ctctags allowclientonlytags="&env.CTC_TAGS_ALLOW_CLIENT_ONLY_TAGS;">
">

Whether to allow client-only message tags (used by IRCv3 clients for metadata).

---

## `<customprefix>` (lines 361–388)

Defines custom channel prefix levels beyond standard `ov`:

| Prefix | Letter | Rank | Set by | Purpose |
|---|---|---|---|---|
| `~` founder | q | 50000 | Services only | Highest rank, cannot be kicked |
| `&` admin | a | 40000 | Services only | Admin level |
| `%` halfop | h | 20000 | +o can set | Limited op |
| `@` op | o | 30000 | +o can set | Standard op |
| `+` voice | v | 10000 | +o can set | Can speak in +m |

The `ranktoset` values control who can set each prefix (e.g. founder rank 50000 required to set admin).

---

## `<deaf>` (lines 390–393)

```inspircd
<deaf bypasschars="&env.DEAF_BYPASS_CHARS;"
       bypasscharsuline="&env.DEAF_BYPASS_CHARS_ULINE;"
       enableprivdeaf="&env.DEAF_ENABLE_PRIV_DEAF;"
       privdeafuline="&env.DEAF_PRIV_DEAF_ULINE;">
">

Controls +d (deaf) and +D (privdeaf) modes:
- `bypasschars` – message prefixes that bypass deaf (e.g. `!` for bot commands)
- `bypasscharsuline` – same but for U-line services
- `enableprivdeaf` – enable private deaf mode (+D)
- `privdeafuline` – U-line bypass for private deaf

---

## `<delaymsg>` (line 395)

```inspircd
<delaymsg allownotice="&env.DELAY_MSG_ALLOW_NOTICE;">
">

When +d (delay message) is set, whether notices bypass the delay.

---

## `<disabled>` (lines 397–400)

```inspircd
<disabled chanmodes="&env.DISABLE_CHMODES;"
          commands="&env.DISABLE_COMMANDS;"
          fakenonexistant="&env.DISABLE_FAKENONEXISTANT;"
          usermodes="&env.DISABLE_USERMODES;">
">

Disables specific commands and modes network-wide:
- `chanmodes` – channel modes users cannot set (empty = none)
- `commands` – commands users cannot run (empty = none)
- `fakenonexistant` – pretend disabled commands don't exist
- `usermodes` – user modes users cannot set (w disabled in this config)

---

## `<dnsbl>` (lines 402–427)

Three DNSBL checks that run on every connection:

| Name | Domain | Records | Action |
|---|---|---|---|
| **DroneBL** | `dnsbl.dronebl.org` | 3,5,6,7,8,9,10,11,13,14,15,16,17,19 | Z-line 5m |
| **EFnet RBL** | `rbl.efnetrbl.org` | 1,2,3,4,5 | Z-line 5m |
| **tor exit** | `torexit.dan.me.uk` | 100 | Z-line 5m |

Each:
- `action` – `zline` (IP ban)
- `duration` – ban length
- `records` – which DNSBL result codes trigger the ban
- `timeout` – DNS query timeout

---

## `<exemptfromfilter>` (lines 429–461)

Exempts service nicknames from the content filter (`m_filter`). If a filter pattern matches a message from these nicks, it is ignored. All standard service names are exempted plus ALIS.

---

## `<hidechans>` (line 463)

```inspircd
<hidechans affectsopers="&env.HIDECHANS_AFFECTS_OPERS;">
">

Whether +I (hide channels in WHOIS) affects opers too.

---

## `<hidelist>` (lines 465–469)

```inspircd
<hidelist mode="filter" rank="30000">
<hidelist mode="invex" rank="0">
```

Hides certain list modes from users below `rank`:
- Channel filter entries (+g) hidden from users below op rank 30000
- Invite exemptions (+I) visible to all (rank 0)

---

## `<hidemode>` (lines 471–472)

```inspircd
<hidemode mode="ban" rank="10000">
```

Hides ban entries from users below voice rank (10000). Normal users see "ban list hidden".

---

## `<hostname>` (line 474)

```inspircd
<hostname charmap="&env.HOSTNAME_CHAR_MAP;">
">

Defines which characters are allowed in hostnames sent to clients. Maps to alphanumeric + `.-_/` for maximum compatibility.

---

## `<httpd>` (line 476)

```inspircd
<httpd timeout="&env.HTTPD_TIMEOUT;">
">

Timeout for HTTP connections to the built-in HTTP daemon (used for stats pages).

---

## `<inviteexception>` (line 478)

```inspircd
<inviteexception bypasskey="&env.INVITE_EXCEPTION_BYPASS_KEY;">
">

Whether invite exceptions (+I) bypass the channel key (+k).

---

## `<ircv3>` (lines 480–482)

```inspircd
<ircv3 accountnotify="&env.IRCV3_ACCOUNT_NOTIFY;"
       awaynotify="&env.IRCV3_AWAY_NOTIFY;"
       extendedjoin="&env.IRCV3_EXTENDED_JOIN;">
">

Enables IRCv3 capabilities:
- `accountnotify` – notify clients when a user's account status changes
- `awaynotify` – notify clients when a user goes away/returns
- `extendedjoin` – send account name and realname in JOIN messages

---

## `<joinflood>` (lines 484–486)

```inspircd
<joinflood bootwait="&env.JOIN_FLOOD_BOOT_WAIT;"
           duration="&env.JOIN_FLOOD_DURATION;"
           splitwait="&env.JOIN_FLOOD_SPLIT_WAIT;">
">

Anti-join-flood settings (used by +j mode):
- `bootwait` – delay after boot before flood detection
- `duration` – how long a join ban lasts
- `splitwait` – delay after netburst before flood detection resumes

---

## `<knock>` (line 488)

```inspircd
<knock notify="&env.KNOCK_NOTIFY;">
">

Who receives `/KNOCK` notifications: `both` = both the target user and the channel ops.

---

## `<messageflood>` (lines 490–492)

```inspircd
<messageflood notice="&env.MESSAGE_FLOOD_NOTICE;"
              privmsg="&env.MESSAGE_FLOOD_PRIVMSG;"
              tagmsg="&env.MESSAGE_FLOOD_TAG_MSG;">
">

Per-second message flood limits used by +f mode. Values are ratios (e.g. `1.0` = 1 line per second per user).

---

## `<monitor>` (line 494)

```inspircd
<monitor maxentries="&env.MONITOR_MAX_ENTRIES;">
">

Maximum monitored nicknames per user (IRCV3 MONITOR system).

---

## `<muteban>` (line 496)

```inspircd
<muteban notifyuser="&env.MUTE_BAN_NOTIFY_USER;">
">

Whether users are notified when a +m (muteban) blocks their message.

---

## `<nickdelay>` (lines 498–499)

```inspircd
<nickdelay delay="&env.NICK_DELAY;" hint="&env.NICK_DELAY_HINT;">
">

Prevents users from reclaiming a disconnected nick for `delay` seconds. `hint` tells them about it.

---

## `<nickflood>` (line 501)

```inspircd
<nickflood duration="&env.NICK_FLOOD_DURATION;">
">

Default nick flood detection duration (used by +F mode).

---

## `<noctcp>` (line 503)

```inspircd
<noctcp enableumode="&env.NO_CTCP_ENABLE_UMODE;">
">

Whether users can set +C (block CTCP) as a user mode.

---

## `<ojoin>` (lines 505–507)

```inspircd
<ojoin notice="&env.OJOIN_NOTICE;" op="&env.OJOIN_OP;"
       prefix="&env.OJOIN_PREFIX;">
">

Controls `/OJOIN` (oper join):
- `notice` – send notice to channel when oper joins
- `op` – auto-op the oper on join
- `prefix` – prefix for the join message

---

## `<operlog>` (line 509)

```inspircd
<operlog tosnomask="&env.OPER_TO_SNOMASK;">
">

Maps oper commands to snomask notifications. `on` means oper actions are logged to the snomask.

---

## `<opermotd>` (lines 511–512)

```inspircd
<opermotd file="/home/irc/oper.motd.txt" onoper="yes">
">

Shows oper MOTD after successful `/OPER`.

---

## `<operprefix>` (line 514)

```inspircd
<operprefix prefix="&env.OPER_PREFIX;">
">

Prefix added to oper nick when they send messages (e.g. `*` = `*nick`).

---

## `<override>` (lines 516–518)

```inspircd
<override enableumode="&env.OVERRIDE_ENABLE_UMODE;"
          noisy="&env.OVERRIDE_NOISY;"
          requirekey="&env.OVERRIDE_REQUIRE_KEY;">
">

Controls +O (override) mode for opers:
- `enableumode` – whether opers can set +O
- `noisy` – announce override actions in channel
- `requirekey` – opers must provide channel key to override +k

---

## `<penalty>` (lines 520–521)

```inspircd
<penalty name="HELPOP" value="60">
">

Sets command penalties (in milliseconds) for rate limiting. `/HELPOP` costs 60ms.

---

## `<permchanneldb>` (lines 523–525)

```inspircd
<permchanneldb filename="/home/irc/permchannels.db"
               listmodes="&env.PERMCHAN_LIST_MODES;"
               saveperiod="&env.PERMCHANDB_SAVE_PERIOD;">
">

Persists permanent channel (+P) modes, bans, and topics to a SQLite database so they survive restarts.

---

## `<remove>` (lines 527–528)

```inspircd
<remove protectedrank="50000"
        supportnokicks="&env.REMOVE_SUPPORT_NO_KICKS;">
">

Controls `/REMOVE`:
- `protectedrank` – minimum rank needed to remove (founder level)
- `supportnokicks` – whether +Q (nokicks) also protects from REMOVE

---

## `<repeat>` (lines 530–534)

```inspircd
<repeat maxbacklog="&env.REPEAT_MAX_BACK_LOG;"
        maxdistance="&env.REPEAT_MAX_DISTANCE;"
        maxlines="&env.REPEAT_MAX_LINES;"
        maxtime="&env.REPEAT_MAX_TIME;"
        size="&env.REPEAT_MAX_SIZE;">
">

Controls +E (repeat message blocking):
- `maxbacklog` – messages to compare against
- `maxdistance` – max character difference to consider a repeat
- `maxlines` – max lines before action triggers
- `maxtime` – time window for detection (0 = unlimited)
- `size` – max message size for comparison

---

## `<rline>` (lines 536–538)

```inspircd
<rline engine="&env.RLINE_ENGINE;"
       matchonnickchange="&env.RLINE_MATCH_ON_NICK_CHANGE;"
       zlineonmatch="&env.RLINE_ZLINE_ON_MATCH;">
">

Regex-based line banning:
- `engine` – regex engine (`pcre`)
- `matchonnickchange` – re-check R-line when nick changes
- `zlineonmatch` – also Z-line on match

---

## `<rotatelog>` (line 540)

```inspircd
<rotatelog period="&env.ROTATE_LOG_PERIOD;">
">

Log rotation period in seconds (`86400` = daily).

---

## `<securelist>` (lines 542–544)

```inspircd
<securelist exemptregistered="&env.SECURE_LIST_EXEMPT_REGISTERED;"
            showmsg="&env.SECURE_LIST_SHOW_MSG;"
            waittime="&env.SECURE_LIST_WAIT_TIME;">
">

Makes `/LIST` require a waiting period before showing channel list:
- `exemptregistered` – registered users skip the wait
- `showmsg` – show a message explaining the wait
- `waittime` – seconds to wait

---

## `<showwhois>` (lines 546–547)

```inspircd
<showwhois opersonly="&env.SHOW_WHOIS_OPER_ONLY;"
           showfromopers="&env.SHOW_WHOIS_FROM_OPERS;">
">

Controls WHOIS notifications (+W mode):
- `opersonly` – only opers can set +W
- `showfromopers` – show WHOIS notifications from opers

---

## `<shun>` (lines 549–554)

```inspircd
<shun affectopers="&env.SHUN_AFFECT_OPERS;"
      allowconnect="&env.SHUN_ALLOW_CONNECT;"
      allowtags="&env.SHUN_ALLOW_TAGS;"
      cleanedcommands="&env.SHUN_CLEANED_COMMANDS;"
      enabledcommands="&env.SHUN_ENABLED_COMMANDS;"
      notifyuser="&env.SHUN_NOTIFY_USER;">
">

Controls shun (restricted mode):
- `affectopers` – whether shuns affect opers
- `allowconnect` – whether shunned users can connect (vs kill)
- `allowtags` – whether shunned users can send IRCv3 tags
- `cleanedcommands` – commands that are silently dropped
- `enabledcommands` – commands shunned users can still use (AWAY, OPER, PING, etc.)
- `notifyuser` – notify shunned user

---

## `<silence>` (lines 556–557)

```inspircd
<silence exemptuline="&env.SILENCE_EXEMPT_ULINE;"
         maxentries="&env.SILENCE_MAX_ENTRIES;">
">

Controls the SILENCE system:
- `exemptuline` – whether U-line servers are exempt from silence
- `maxentries` – max silence entries per user

---

## `<sslinfo>` (line 559)

```inspircd
<sslinfo operonly="&env.SSL_INFO_OPER_ONLY;">
">

Whether `/SSLINFO` (TLS certificate info) is oper-only.

---

## `<sslmodes>` (line 561)

```inspircd
<sslmodes enableumode="&env.SSL_ENABLE_UMODE;">
">

Whether users can set +z (TLS-only PMs) as a user mode.

---

## `<stdregex>` (line 563)

```inspircd
<stdregex type="&env.REGEX_TYPE;">
">

Default regex engine type for the server: `ecmascript` (JavaScript-compatible regex).

---

## `<svshold>` (line 565)

```inspircd
<svshold silent="&env.SVS_HOLD_SILENT;">
">

Whether SVS hold (nick locking by services) is silent (no notice to user).

---

## `<timedbans>` (line 567)

```inspircd
<timedbans sendnotice="&env.TIMED_BANS_SEND_NOTICE;">
">

Whether users are notified when a timed ban expires.

---

## `<uline>` (lines 569–570)

```inspircd
<uline server="&env.SERVICES_ULINE;" silent="no">
">

Declares a server as a **U-line** (services server). U-lines are exempt from many restrictions. `silent=no` means U-line actions are visible in snomask logs.

---

## `<waitpong>` (lines 572–573)

```inspircd
<waitpong killonbadreply="&env.WAIT_PONG_KILL_ON_BAD_REPLY;"
          sendsnotice="&env.WAIT_PONG_SEND_NOTICE;">
">

Controls connection wait-pong (used during initial connection):
- `killonbadreply` – kill user if PONG is wrong
- `sendsnotice` – send notice on wait-pong events

---

## `<watch>` (line 575)

```inspircd
<watch maxwatch="&env.WATCH_MAX;">
">

Maximum watched nicknames per user (legacy WATCH system).

---

## `<wsorigin>` (line 577)

```inspircd
<wsorigin allow="&env.WS_ORIGIN_ALLOW;">
">

Allowed WebSocket origins for the websocket module. Restricts which web domains can connect via WS.

---

## `<xlinedb>` (lines 579–580)

```inspircd
<xlinedb filename="/home/irc/xline.db"
         saveperiod="&env.XLINEDB_SAVE_PERIOD;">
">

Persists X-lines (all ban types) to a SQLite database every `saveperiod` seconds.

---

## `<zombie>` (lines 582–585)

```inspircd
<zombie cleansplit="&env.ZOMBIE_CLEAN_SPLIT;"
        dirtysplit="&env.ZOMBIE_DIRTY_SPLIT;"
        maxzombies="&env.ZOMBIE_MAX;"
        serverzombietime="&env.ZOMBIE_SERVER_TIME;">
">

Controls zombie user cleanup after a netburst/server split:
- `cleansplit` – remove zombies from clean splits
- `dirtysplit` – remove zombies from dirty splits
- `maxzombies` – max zombie users before cleanup
- `serverzombietime` – how long before a server's users become zombies
