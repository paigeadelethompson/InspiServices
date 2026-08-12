// InspiServices - database schema (created on first run).
#include "services/db.h"

namespace svc {

  void db_schema(db &d) {
    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS config (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT '',
			updated_at INTEGER DEFAULT 0
		);
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS nickserv (
			name       TEXT PRIMARY KEY,   -- folded nick
			password   TEXT NOT NULL,      -- pbkdf2($salt,$iter) hex
			salt       TEXT NOT NULL,
			email      TEXT DEFAULT '',
			registered INTEGER NOT NULL,
			lastseen   INTEGER DEFAULT 0,
			messages   INTEGER DEFAULT 0,
			account    TEXT NOT NULL,      -- account/pseudonym
			hidden     INTEGER DEFAULT 0,
			umodes     TEXT DEFAULT ''     -- usermodes applied on identify
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS chanserv (
			name     TEXT PRIMARY KEY,      -- channel with '#' prefix
			founder  TEXT NOT NULL,         -- folded nick
			password TEXT NOT NULL,         -- pbkdf2
			salt     TEXT NOT NULL,
			modes    TEXT DEFAULT '+nt',
			topic    TEXT DEFAULT '',
			registered INTEGER NOT NULL,
			lastused   INTEGER DEFAULT 0
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS chanserv_access (
			channel TEXT NOT NULL,
			who     TEXT NOT NULL,          -- folded nick or mask
			level   INTEGER NOT NULL,
			PRIMARY KEY (channel, who)
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS chanserv_akick (
			channel TEXT NOT NULL,
			who     TEXT NOT NULL,
			by      TEXT NOT NULL,
			ts      INTEGER NOT NULL,
			reason  TEXT DEFAULT ''
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS botserv (
			name      TEXT PRIMARY KEY,     -- classified by the BotServ subservices
			assigned  TEXT NOT NULL,        -- channel or "" for unassigned
			nick      TEXT NOT NULL,
			realname  TEXT DEFAULT '',
			password  TEXT DEFAULT '',
			reply     TEXT NOT NULL DEFAULT 'NOTICE',  -- NOTICE|CHANNEL
			fop       TEXT NOT NULL DEFAULT '',         -- '' == '!' fantasy trigger
			chan_nick TEXT NOT NULL DEFAULT ''          -- per-channel nick override
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS operserv (
			nick     TEXT PRIMARY KEY,       -- folded oper nick
			login    TEXT NOT NULL,
			level    TEXT NOT NULL DEFAULT 'admin',
			password TEXT NOT NULL,          -- pbkdf hash
			salt     TEXT NOT NULL
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS bridges (
			name      TEXT PRIMARY KEY,
			kind      TEXT NOT NULL DEFAULT 'link',
			server_host TEXT NOT NULL,
			port      TEXT NOT NULL DEFAULT '6697',
			token_env TEXT NOT NULL,
			account   TEXT NOT NULL DEFAULT '',
			send_tls  INTEGER NOT NULL DEFAULT 1,
			enabled   INTEGER NOT NULL DEFAULT 1
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS bridge_channels (
			bridge  TEXT NOT NULL,
			channel TEXT NOT NULL,       -- IRC channel, e.g. #support
			remote  TEXT NOT NULL,       -- discord channel id / signal number or groupId
			PRIMARY KEY (bridge, channel)
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS linksecret (
			name     TEXT PRIMARY KEY,
			value    TEXT NOT NULL
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS pending_reg (
			name       TEXT PRIMARY KEY,   -- folded nick
			password   TEXT NOT NULL,      -- pbkdf2 hash
			email      TEXT DEFAULT '',
			requested  INTEGER NOT NULL,   -- unix timestamp
			approved   INTEGER DEFAULT 0,  -- 0=pending, 1=approved, -1=rejected
			approved_by TEXT DEFAULT ''     -- oper nick who approved/rejected
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS global (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS pending_chan (
			name       TEXT PRIMARY KEY,   -- channel name
			founder    TEXT NOT NULL,      -- folded account of requester
			requested  INTEGER NOT NULL,   -- unix timestamp
			approved   INTEGER DEFAULT 0,  -- 0=pending, 1=approved, -1=rejected
			approved_by TEXT DEFAULT ''
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS oper_groups (
			name       TEXT PRIMARY KEY,   -- group name, e.g. "opers"
			privileges TEXT NOT NULL DEFAULT ''  -- comma-separated privs
		)
	)SQL");

    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS oper_group_users (
			grp TEXT NOT NULL,
			who TEXT NOT NULL,             -- folded account or nick
			PRIMARY KEY (grp, who)
		)
	)SQL");

    // TLS client certificate fingerprints bound to a registered nickname.
    // A user connecting with a matching certfp is identified automatically.
    d.run(R"SQL(
		CREATE TABLE IF NOT EXISTS nickserv_cert (
			name   TEXT NOT NULL,          -- folded registered nick
			certfp TEXT NOT NULL,          -- SHA-256 fingerprint hex
			PRIMARY KEY (name, certfp)
		)
	)SQL");

    d.run(
        "CREATE INDEX IF NOT EXISTS ix_chanserv_founder ON chanserv(founder)");
    d.run("CREATE INDEX IF NOT EXISTS ix_access_channel ON "
          "chanserv_access(channel)");
    d.run("CREATE INDEX IF NOT EXISTS ix_akick_channel ON "
          "chanserv_akick(channel)");
    d.run("CREATE INDEX IF NOT EXISTS ix_ogrp_user ON "
          "oper_group_users(who)");

    // Column migrations for databases created before the per-channel BotServ
    // settings were introduced.
    auto botcols = d.query("PRAGMA table_info(botserv)");
    bool has_reply = false, has_fop = false, has_chan_nick = false;
    for (auto const &r : botcols) {
      std::string const nm = r.as_string("name");
      if (nm == "reply")
        has_reply = true;
      else if (nm == "fop")
        has_fop = true;
      else if (nm == "chan_nick")
        has_chan_nick = true;
    }
    if (!has_reply)
      d.run("ALTER TABLE botserv ADD COLUMN reply TEXT NOT NULL DEFAULT "
            "'NOTICE'");
    if (!has_fop)
      d.run("ALTER TABLE botserv ADD COLUMN fop TEXT NOT NULL DEFAULT ''");
    if (!has_chan_nick)
      d.run("ALTER TABLE botserv ADD COLUMN chan_nick TEXT NOT NULL DEFAULT ''");

    auto nscols = d.query("PRAGMA table_info(nickserv)");
    bool has_umodes = false;
    for (auto const &r : nscols)
      if (r.as_string("name") == "umodes")
        has_umodes = true;
    if (!has_umodes)
      d.run("ALTER TABLE nickserv ADD COLUMN umodes TEXT DEFAULT ''");
  }

} // namespace svc