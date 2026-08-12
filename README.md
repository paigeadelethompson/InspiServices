# InspiServices

A modular IRC services daemon for InspIRCd, providing NickServ, ChanServ, BotServ, OperServ, and BridgeServ (Discord, Signal, InspIRCd LINK). Written in C++.

## Build

```sh
cmake -S . -B build
cmake --build -j$(nproc) build
```

Output binary: `build/inspiservices`

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
   build/inspiservices --env env/myenv
   ```
3. Link InspIRCd to the services port (see `examples/inspircd/custom.conf`).

## Configuration

All configuration is done via environment variables (see `env/.env`). Key variables:

| Variable | Description |
|---|---|
| `SERVICES_HOST` / `SERVICES_PORT` | Bind address for server link |
| `SERVER_NAME` | U-line server name sent to IRCd |
| `LINK_RECV_PASSWORD` / `LINK_SEND_PASSWORD` | Link passwords |
| `DB_PATH` | SQLite database path (default `inspiservices.db`) |
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

The daemon creates a SQLite database (`inspiservices.db`) with tables for accounts, channels, bots, bans, and bridge mappings. See `src/services/schema.cpp` for the full DDL.

## Theory of Operation

### Distinct SIDs, Not Partitioned

Two networks with different `NETWORK=networkname` can coexist as long as every server has a distinct SID — no partitioning of the SID space is required. Each server/hub picks any unused SID from the 12,960 available.

### Per-Row Network Tag

Every row in every table of the services database carries a **network name column** — the `NETWORK` value of the network that created that row. Data from multiple networks coexists in the same database, tagged by origin. Replication is not needed — instead, all databases are kept online and reachable so that every add operation checks that another network hasn't already claimed a nick, channel, etc.

**Example:**
- A user registered on network `alpha` (hub SID `01A`) connects via a server on network `beta` (hub SID `11A`).
- The user identifies to NickServ.
- They register a channel `#example`.
- The channel row stores `alpha` as its network — it remains an `alpha` asset even though the user connected through `beta`.
- If `beta` later splits off, it deletes all rows tagged `beta`, leaving `alpha` data intact.

### Relation to Bridge SID Limit

The bridge virtual link limit (~12,960 SIDs) is shared across all networks. Each network consumes SIDs for its hub, services daemon, and every bridged link (Discord channel, Signal group, etc.). Distinct SIDs per server/link is the only requirement — no partitioning needed.

## Examples

- `env/.env` – Template with all configurable variables
- `examples/inspircd/` – InspIRCd config, TLS cert generation script, and `config.env` for a hub/leaf setup

## License

MIT
