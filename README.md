# AnswerServices

A modular IRC services daemon for InspIRCd, providing NickServ, ChanServ, BotServ, OperServ, and BridgeServ (Discord, Signal, InspIRCd LINK). Written in C++.

## Build

```sh
cmake -S . -B build
cmake --build -j$(nproc) build
```

Output binary: `build/anservices`

## Dependencies

- OpenSSL (TLS)
- SQLite3 (database)
- pthreads
- jsoncpp (vendored in `3rdparty/`)

Install on Debian/Ubuntu:

```sh
apt install cmake build-essential libssl-dev libsqlite3-dev
```

## Quick Start

1. Create an environment file from the template:
   ```sh
   cp env/.env env/myenv
   # edit env/myenv to set passwords, hosts, etc.
   ```
2. Run the daemon:
   ```sh
   build/anservices --env env/myenv
   ```
3. Link InspIRCd to the services port (see `examples/inspircd/custom.conf`).

## Configuration

All configuration is done via environment variables (see `env/.env`). Key variables:

| Variable | Description |
|---|---|
| `SERVICES_HOST` / `SERVICES_PORT` | Bind address for server link |
| `SERVER_NAME` | U-line server name sent to IRCd |
| `LINK_RECV_PASSWORD` / `LINK_SEND_PASSWORD` | Link passwords |
| `DB_PATH` | SQLite database path (default `anservices.db`) |
| `BRIDGE_DISCORD_TOKEN` | Discord bot token |
| `BRIDGE_SIGNAL_PATH` | Signal CLI socket path |

Service identities are configured per-environment via `NS_*`, `CS_*`, `BS_*`, `OS_*`, `BRS_*` variables (see `src/services/services.cpp` for the full list).

## Service Modules

All commands are available to IRC operators via `/msg <Service> help`.

### NickServ (NS)
- `NS REGISTER <nick> <password>` – Register a nick
- `NS IDENTIFY <nick> <password>` – Authenticate
- `NS SET <option> <value>` – Change settings (email, kill, etc.)
- `NS INFO <nick>` – View account info
- `NS LIST` – List registered nicks

### ChanServ (CS)
- `CS REGISTER <#channel>` – Register a channel
- `CS SET <#channel> <option> <value>` – Change channel settings
- `CS INFO <#channel>` – View channel info
- `CS OP <#channel>` – Gain operator status

### BotServ (BS)
- `BS BOT add <botnick> <password>` – Add a bot
- `BS BOT del <botnick>` – Remove a bot
- `BS ASSIGN <botnick> <#channel>` – Assign a bot to a channel
- `BS UNASSIGN <botnick>` – Unassign a bot
- `BS SET <botnick> <option> <value>` – Change bot settings

### OperServ (OS)
- `OS MODLOGIN <nick>` – Force-module login for a user
- `OS GETPASS <nick>` – Retrieve password hash
- `OS FORCE <nick> <command>` – Execute a command as another user
- `OS BAN add <mask>` / `OS BAN del <mask>` – Manage bans
- `OS JUMP <server>` – Link to a new server

### BridgeServ (BRS)
- `BRS LINK <server> [sendpass] [recvpass]` – Link to another IRCd via InspIRCd LINK protocol
- `BRS DISCORD <action>` – Bridge a Discord channel
- `BRS SIGNAL <action>` – Bridge a Signal group

## Database Schema

The daemon creates a SQLite database (`anservices.db`) with tables for accounts, channels, bots, bans, and bridge mappings. See `src/services/schema.cpp` for the full DDL.

## Theory of Operation

### SID Partitioning Across Networks

InspIRCd SIDs are 3 bytes: `NNA` (digit-digit-letter) or `NNN` (digit-digit-digit). The first byte (the most-significant nibble) identifies the **network number** (0–9 = 10 unique co-operating networks). The remaining two bytes identify servers within that network.

- Network 0: `0NN`, `0NA` — partner A
- Network 1: `1NN`, `1NA` — partner B
- Network 2: `2NN`, `2NA` — partner C
- ...
- Network 9: `9NN`, `9NA` — partner H

Each network owns a `/10` prefix of the SID space, giving it up to `360` servers (`10×10 + 10×26 = 100+260 = 360`).

### Per-Row Network SID

Every row in every table of the services database carries a **network SID column** — the SID of the hub server for the network that created that row. This enables multimaster replication: data from multiple networks coexists in the same database, tagged by origin.

**Example:**
- A user registered on network `01A` (network 0, server 1A) connects via a server on network `11A` (network 1, server 1A).
- The user identifies to NickServ.
- They register a channel `#example`.
- The channel row stores `01A` as its network SID — it remains a `01A` asset even though the user connected through `11A`.
- If network 1 later splits off, it deletes all rows tagged `1NN`/`1NA`, leaving `0NN`/`0NA` data intact.

### Relation to Bridge SID Limit

The bridge virtual server limit (~12,960 SIDs) is shared across all 10 networks. Each network consumes SIDs for its hub, services daemon, and any bridged guilds (Discord, Signal, etc.). The partitioning scheme ensures no two networks collide on SID space.

## Examples

- `env/.env` – Template with all configurable variables
- `examples/inspircd/` – InspIRCd config, TLS cert generation script, and `config.env` for a hub/leaf setup

## License

MIT
