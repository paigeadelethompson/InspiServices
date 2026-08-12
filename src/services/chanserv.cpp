// InspiServices - ChanServ: channel registration, operator status helpers,
// AKICK enforcement, and per-user access.
#include "services/irc/protocol.h"
#include "services/services/core.h"
#include "services/services/modules.h"
#include "services/services/syntax.h"
#include "services/util/log.h"
#include "services/util/util.h"

#include <algorithm>
#include <ctime>

namespace svc::core {

  namespace {

    struct chrec {
      bool exists = false;
      std::string founder;
      std::string password;
      std::string modes;
      std::string topic;
      std::int64_t registered = 0;
      std::int64_t lastused = 0;
    };

    chrec load_channel(ctx &c, std::string_view name) {
      chrec r;
      auto rows = c.database().query(
          "SELECT founder, password, modes, topic, registered, lastused FROM "
          "chanserv WHERE name=? LIMIT 1",
          {std::string(sv::irc_lower(name))});
      if (rows.empty())
        return r;
      r.exists = true;
      r.founder = rows[0].as_string("founder");
      r.password = rows[0].as_string("password");
      r.modes = rows[0].as_string("modes");
      r.topic = rows[0].as_string("topic");
      r.registered = rows[0].as_int("registered");
      r.lastused = rows[0].as_int("lastused");
      return r;
    }

    // Maps a named role to an access level (case-insensitive). Returns -1 for
    // anything that is not a known role name.
    int role_level(std::string_view name) {
      if (sv::equals_ci(name, "VOICE"))
        return 200;
      if (sv::equals_ci(name, "HOP") || sv::equals_ci(name, "HALFOP"))
        return 250;
      if (sv::equals_ci(name, "OP") || sv::equals_ci(name, "SOP"))
        return 300;
      if (sv::equals_ci(name, "PROTECT") || sv::equals_ci(name, "ADMIN"))
        return 400;
      if (sv::equals_ci(name, "OWNER") || sv::equals_ci(name, "FOUNDER"))
        return 600;
      return -1;
    }

    // Human label for a level.
    std::string level_role(int lv) {
      if (lv >= 600)
        return "OWNER";
      if (lv >= 400)
        return "PROTECT";
      if (lv >= 300)
        return "OP";
      if (lv >= 250)
        return "HOP";
      if (lv >= 200)
        return "VOICE";
      return "NONE";
    }

    // Effective (highest) access level the caller has on a channel; the
    // founder always counts as OWNER (600).
    int caller_level(ctx &c, cmsg const &m, std::string_view chan) {
      irc::user *u = c.net().find_user(m.sender);
      if (!u)
        return -100;
      std::string const founder = channel_founder(c, chan);
      if (!founder.empty() && !u->account.empty() &&
          founder == fold(u->account))
        return 600;
      int lv = channel_access_level(c, chan, u->nick);
      if (!u->account.empty())
        lv = std::max(lv, channel_access_level(c, chan, u->account));
      return lv;
    }

    // True if the sender has at least `min` access on `chan` (founder always
    // passes).
    bool has_access(ctx &c, cmsg const &m, std::string_view chan, int min) {
      irc::user *u = c.net().find_user(m.sender);
      return u != nullptr && can_chan(c, *u, chan, min);
    }

    void send_topic(ctx &c, cmsg const &m, std::string_view chan,
                    std::string_view topic) {
      irc::message mk;
      mk.prefix = m.service->uid;
      mk.command = "TOPIC";
      mk.params.push_back(std::string(chan));
      mk.params.push_back(std::string(topic));
      c.deliver(mk);
    }

    // Case-insensitive "*"/"?" wildcard match (RFC1459 folding).
    bool wcmatch_ci(std::string_view pat, std::string_view text) {
      std::string const p = sv::irc_lower(pat);
      std::string const t = sv::irc_lower(text);
      std::size_t pi = 0, ti = 0;
      std::size_t star = std::string::npos, mark = 0;
      while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
          ++pi;
          ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
          star = pi++;
          mark = ti;
        } else if (star != std::string::npos) {
          pi = star + 1;
          ti = ++mark;
        } else
          return false;
      }
      while (pi < p.size() && p[pi] == '*')
        ++pi;
      return pi == p.size();
    }

    // True if a channel AKICK entry matches a user. Entries containing '!' or
    // '@' are hostmasks matched against nick!user@host; anything else matches
    // the user's nick or (folded) account.
    bool akick_match(std::string const &pat, irc::user const &u) {
      if (pat.find('!') != std::string::npos ||
          pat.find('@') != std::string::npos)
        return wcmatch_ci(pat, u.fullmask());
      if (!u.account.empty() && wcmatch_ci(pat, u.account))
        return true;
      return wcmatch_ci(pat, u.nick);
    }

    std::string ts_str(std::int64_t ts) {
      if (ts <= 0)
        return "unknown";
      std::time_t const t = static_cast<std::time_t>(ts);
      std::tm tm{};
      if (!gmtime_r(&t, &tm))
        return "unknown";
      char buf[64];
      std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
      return buf;
    }

    // Bans `u`'s host, removes them from the channel and serves the KICK.
    void akick_apply(ctx &c, irc::channel &ch, irc::user const &u,
                     std::string const &entry, std::string const &reason) {
      std::string const svc = c.service_uid("ChanServ");
      if (svc.empty())
        return;
      std::string const mask = entry.find('@') == std::string::npos
                                   ? std::string("*!*@") + u.displayhost
                                   : entry;
      irc::message m;
      m.prefix = svc;
      m.command = "FMODE";
      m.params.push_back(std::string(ch.name));
      std::int64_t const ts = ch.modelock > 0 ? ch.modelock : svc::irc::now();
      m.params.push_back(std::to_string(ts));
      m.params.push_back("+b");
      m.params.push_back(mask);
      c.deliver(m);

      irc::message k;
      k.prefix = svc;
      k.command = "KICK";
      k.params.push_back(std::string(ch.name));
      k.params.push_back(u.uid);
      k.params.push_back(reason.empty() ? std::string("Banned")
                                        : reason + " (AUTO-KICK)");
      c.deliver(k);
      ch.members.erase(u.uid);
      log::info("cs", "AKICK {} removed {} ({} {})", ch.name, u.nick, entry,
                reason.empty() ? "banned" : reason);
    }

    // Checks the channel's AKICK list for a user that just joined.
    void enforce_akick(ctx &c, irc::user const &u, irc::channel &ch) {
      auto rows = c.database().query(
          "SELECT who, reason FROM chanserv_akick WHERE channel=? ORDER BY who",
          {std::string(sv::irc_lower(ch.name))});
      if (rows.empty())
        return;
      for (auto const &r : rows) {
        if (!akick_match(r.as_string("who"), u))
          continue;
        akick_apply(c, ch, u, r.as_string("who"), r.as_string("reason"));
        return;
      }
    }

    // Status-mode commands share one body: OP/DEOP, VOICE/DEVOICE,
    // HOP/DEHOP, PROTECT/DEPROTECT.
    struct status_cmd {
      char const *name;
      bool add;
      char mode;
      int need;
    };
    status_cmd const status_cmds[] = {
        {"OP", true, 'o', 300},      {"DEOP", false, 'o', 300},
        {"VOICE", true, 'v', 200},   {"DEVOICE", false, 'v', 200},
        {"HOP", true, 'h', 250},     {"DEHOP", false, 'h', 250},
        {"PROTECT", true, 'a', 400}, {"DEPROTECT", false, 'a', 400},
    };

    // Named channel-mode features for SET (KEEPtable mirrors the server's
    // CHANMODES advertisement).
    struct feature {
      char const *name;
      char mode;
      bool takes_value;
    };
    feature const features[] = {
        {"INVITEONLY", 'i', false},   {"MODERATED", 'm', false},
        {"NOEXTMSG", 'n', false},     {"NOEXTERNAL", 'n', false},
        {"TOPICLOCK", 't', false},    {"PRIVATE", 'p', false},
        {"SECRET", 's', false},       {"REGINVITE", 'R', false},
        {"REGMODERATED", 'M', false}, {"ALLOWINVITE", 'A', false},
        {"OPERONLY", 'O', false},     {"SSLONLY", 'z', false},
        {"OPMODERATED", 'U', false},  {"STRIPCOLOR", 'S', false},
        {"BLOCKCOLOR", 'c', false},   {"NOCTCP", 'C', false},
        {"NONOTICE", 'T', false},     {"NONICK", 'N', false},
        {"NOKNOCK", 'K', false},      {"NOKICK", 'Q', false},
        {"AUDITORIUM", 'u', false},   {"PERMANENT", 'P', false},
        {"DELAYJOIN", 'D', false},    {"REGISTERED", 'r', false},
        {"KEY", 'k', true},           {"LIMIT", 'l', true},
        {"FLOOD", 'f', true},         {"JOINFLOOD", 'j', true},
        {"NICKFLOOD", 'F', true},     {"REPEAT", 'E', true},
        {"HISTORY", 'H', true},       {"DELAYMSG", 'd', true},
        {"KICKNOREEJOIN", 'J', true}, {"REDIRECT", 'L', true},
        {"ANTICAPS", 'B', true},
    };

    feature const *feature_lookup(std::string_view name) {
      for (auto const &f : features)
        if (sv::equals_ci(name, f.name))
          return &f;
      return nullptr;
    }

  } // namespace

  void install_chanserv(ctx &c) {
    c.add_help("chanserv", "REGISTER",
               "Usage: REGISTER <#channel>\n"
               "Registers a channel to your (identified) account. You must be "
               "logged in with NickServ first. May require operator approval.");
    c.add_help("chanserv", "OP",
               "Usage: OP <#channel> [nick]\n"
               "Gives op status to a user (defaults to you). Requires "
               "founder-level access (300+).");
    c.add_help("chanserv", "DEOP",
               "Usage: DEOP <#channel> [nick]\n"
               "Removes op status from a user.");
    c.add_help("chanserv", "VOICE",
               "Usage: VOICE <#channel> [nick]\n"
               "Voices a user in the channel (200+).");
    c.add_help("chanserv", "DEVOICE",
               "Usage: DEVOICE <#channel> [nick]\n"
               "Removes voice from a user.");
    c.add_help("chanserv", "PROTECT",
               "Usage: PROTECT <#channel> [nick]\n"
               "Gives protected (halfop/owner) status to a user (400+).");
    c.add_help("chanserv", "DEPROTECT",
               "Usage: DEPROTECT <#channel> [nick]\n"
               "Removes protected status from a user.");
    c.add_help("chanserv", "HOP",
               "Usage: HOP <#channel> [nick]\n"
               "Gives halfop status to a user (250+).");
    c.add_help("chanserv", "DEHOP",
               "Usage: DEHOP <#channel> [nick]\n"
               "Removes halfop status from a user.");
    c.add_help("chanserv", "MODE",
               "Usage: MODE <#channel> <modes> [params]\n"
               "Applies channel modes (e.g. +nt), forwarded from ChanServ.");
    c.add_help("chanserv", "TOPIC",
               "Usage: TOPIC <#channel> <topic>\n"
               "Sets the channel topic.");
    c.add_help("chanserv", "KICK",
               "Usage: KICK <#channel> <nick> [reason]\n"
               "Kicks a user from the channel (500+).");
    c.add_help("chanserv", "BAN",
               "Usage: BAN <#channel> <mask> [reason]\n"
               "Bans a nick/host mask (400+).");
    c.add_help("chanserv", "UNBAN",
               "Usage: UNBAN <#channel> <mask>\n"
               "Removes a ban mask.");
    c.add_help("chanserv", "AKICK",
               "Usage: AKICK <#channel> LIST\n"
               "       AKICK <#channel> ADD <mask> [reason]\n"
               "       AKICK <#channel> DEL <mask>\n"
               "Manages the auto-kick list. Users matching an entry are "
               "banned and kicked the moment they join. Masks may be a nick, "
               "an account, or a hostmask (300+ to change).");
    c.add_help("chanserv", "ACCESS",
               "Usage: ACCESS <#channel> LIST\n"
               "       ACCESS <#channel> ADD <who> <ROLE|level> [level]\n"
               "       ACCESS <#channel> DEL <who>\n"
               "Manages per-user channel permissions (roles): VOICE=200, "
               "HOP/HALFOP=250, OP/SOP=300, PROTECT/ADMIN=400, OWNER=600, or "
               "a numeric level. Levels gate which commands a user may use "
               "(e.g. OP needs 300+, BAN 400+, KICK 500+); the founder is "
               "always OWNER. Only users with 500+ may change the list, and "
               "no one may give or take away access matching or exceeding "
               "their own.");
    c.add_help(
        "chanserv", "SET",
        "Usage: SET <#channel> PASSWORD <password>\n"
        "       SET <#channel> FOUNDER <nick>\n"
        "       SET <#channel> <FEATURE> [value]        (PROTECT+, 400)\n"
        "       SET <#channel> NO<FEATURE> | -<FEATURE>\n"
        "Toggles the channel mode features the network advertises. "
        "Feature names (channel mode): INVITEONLY(i), MODERATED(m), "
        "NOEXTMSG(n), TOPICLOCK(t), PRIVATE(p), SECRET(s), "
        "REGINVITE(R), REGMODERATED(M), ALLOWINVITE(A), OPERONLY(O), "
        "SSLONLY(z), OPMODERATED(U), STRIPCOLOR(S), BLOCKCOLOR(c), "
        "NOCTCP(C), NONOTICE(T), NONICK(N), NOKNOCK(K), NOKICK(Q), "
        "AUDITORIUM(u), PERMANENT(P), DELAYJOIN(D), REGISTERED(r). "
        "Value modes: KEY <key>, LIMIT <n>, FLOOD/ANTICAPS/REPEAT "
        "<config>, JOINFLOOD/NICKFLOOD <n:t>, HISTORY <n>, DELAYMSG "
        "<n>, KICKNOREEJOIN <n>, REDIRECT <#chan>.");
    c.add_help("chanserv", "STATS",
               "Usage: STATS <#channel>\n"
               "Shows channel registration and access summary.");
    c.add_help("chanserv", "DROP",
               "Usage: DROP <#channel>\n"
               "Drops the channel registration (founder only).");

    // REGISTER <#channel>
    c.on_command("chanserv", "REGISTER", [](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: REGISTER <#channel>");
        return;
      }
      std::string const &chan = m.arg(0);
      if (!sv::valid_chan(chan)) {
        c.notice(m, "Invalid channel name.");
        return;
      }
      irc::user *u = c.net().find_user(m.sender);
      if (!u || u->account.empty()) {
        c.notice(
            m,
            "You must be logged in (NickServ) before registering a channel.");
        return;
      }
      chrec r = load_channel(c, chan);
      if (r.exists) {
        c.notice(m, "Channel '" + chan + "' is already registered.");
        return;
      }
      std::string const key = sv::irc_lower(chan);

      auto pend = c.database().query(
          "SELECT approved FROM pending_chan WHERE name=? LIMIT 1", {key});
      if (!pend.empty() && pend[0].as_int("approved") == 0) {
        c.notice(m, "Registration for channel '" + chan +
                        "' is already pending operator approval.");
        return;
      }

      std::int64_t const now = svc::irc::now();
      bool auto_approve = false;
      auto g = c.database().query(
          "SELECT value FROM global WHERE key='auto_approve_chan' LIMIT 1");
      if (!g.empty())
        auto_approve = g[0].as_string("value") == "1";

      if (auto_approve) {
        activate_channel(c, chan, u->account);
        c.notice(m,
                 "Channel '" + chan + "' registered to '" + u->account + "'.");
        log::info("cs", "{} registered {} (auto)", u->account, chan);
      } else {
        // REPLACE so a previously rejected entry can be re-submitted.
        c.database().run(
            "INSERT OR REPLACE INTO pending_chan (name, founder, requested, "
            "approved) VALUES (?, ?, ?, 0)",
            {key, fold(u->account), now});
        c.notice(m, "Registration for channel '" + chan +
                        "' submitted and is pending operator approval.");
        log::info("cs", "{} submitted channel registration for {}", u->account,
                  chan);
      }
    });

    // OP/DEOP/VOICE/DEVOICE/HOP/DEHOP/PROTECT/DEPROTECT.
    for (auto const &sc : status_cmds) {
      c.on_command("chanserv", sc.name, [sc](ctx &c, cmsg const &m) {
        std::string const cmd(sc.name);
        if (m.argc() < 1) {
          c.notice(m, "Usage: " + cmd + " <#channel> [nick]");
          return;
        }
        std::string const &chan = m.arg(0);
        std::string nick = m.arg(1);
        if (nick.empty())
          nick = m.nick;
        irc::user *tu = c.net().by_nick(nick);
        if (!tu) {
          c.notice(m, "No such user '" + nick + "'.");
          return;
        }
        if (!has_access(c, m, chan, sc.need)) {
          if (sc.add)
            c.notice(m, "You do not have access to set " + cmd + " in '" +
                            chan + "'.");
          else
            c.notice(m, "You do not have access to remove " +
                            std::string(cmd, 2) + " in '" + chan + "'.");
          return;
        }
        send_chan_mode(c, c.service_uid("ChanServ"), chan,
                       std::string(1, sc.add ? '+' : '-') + sc.mode, tu->uid);
      });
    }

    // ---- AKICK: managed auto-kick list, enforced on join ----
    c.on_command("chanserv", "AKICK", [](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: AKICK <#channel> LIST|ADD <mask> [reason]|DEL "
                    "<mask>");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &op = m.arg(1);
      if (op.empty() || sv::equals_ci(op, "LIST")) {
        if (!has_access(c, m, chan, 100)) {
          c.notice(m, "You do not have access to '" + chan + "'.");
          return;
        }
        auto rows = c.database().query(
            "SELECT who, reason, ts FROM chanserv_akick WHERE channel=? "
            "ORDER BY who",
            {std::string(sv::irc_lower(chan))});
        if (rows.empty()) {
          c.notice(m, "No AKICK entries for " + chan + ".");
          return;
        }
        c.notice(m, "AKICK list for " + chan + ":");
        for (auto const &r : rows) {
          std::string const why = r.as_string("reason");
          c.notice(m, "  " + r.as_string("who") +
                          (why.empty() ? std::string()
                                       : std::string("  --  ") + why));
        }
        return;
      }
      if (sv::equals_ci(op, "ADD")) {
        if (m.argc() < 3) {
          c.notice(m, "Usage: AKICK <#channel> ADD <mask> [reason]");
          return;
        }
        if (!has_access(c, m, chan, 300)) {
          c.notice(m, "You do not have permission to modify the AKICK list.");
          return;
        }
        std::string const &mask = m.arg(2);
        std::string const reason = m.arg(3);
        std::string const why =
            reason.empty() ? std::string() : fold(m.nick) + ": " + reason;
        std::int64_t const now = svc::irc::now();
        c.database().run("DELETE FROM chanserv_akick WHERE channel=? AND who=?",
                         {std::string(sv::irc_lower(chan)), mask});
        c.database().run("INSERT INTO chanserv_akick (channel, who, by, ts, "
                         "reason) VALUES (?, ?, ?, ?, ?)",
                         {std::string(sv::irc_lower(chan)), mask,
                          std::string(m.sender), now, why});
        c.notice(m, "Added " + mask + " to the AKICK list for " + chan + ".");
        // Enforce immediately against anyone currently matching.
        if (irc::channel *ch = c.net().find_channel(chan)) {
          std::vector<irc::user *> todrop;
          for (auto const &[uid, _] : ch->members) {
            if (irc::user *u = c.net().find_user(uid);
                u && akick_match(mask, *u))
              todrop.push_back(u);
          }
          for (irc::user *u : todrop)
            akick_apply(c, *ch, *u, mask, reason);
        }
        return;
      }
      if (sv::equals_ci(op, "DEL")) {
        if (m.argc() < 3) {
          c.notice(m, "Usage: AKICK <#channel> DEL <mask>");
          return;
        }
        if (!has_access(c, m, chan, 300)) {
          c.notice(m, "You do not have permission to modify the AKICK list.");
          return;
        }
        c.database().run("DELETE FROM chanserv_akick WHERE channel=? AND who=?",
                         {std::string(sv::irc_lower(chan)), m.arg(2)});
        c.notice(m, "Removed " + m.arg(2) + " from the AKICK list.");
        return;
      }
      c.notice(m, "Unknown AKICK operation. Use LIST, ADD, or DEL.");
    });

    // ---- ChanServ STATS ----
    c.on_command("chanserv", "STATS", [](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: STATS <#channel>");
        return;
      }
      std::string const &chan = m.arg(0);
      chrec r = load_channel(c, chan);
      if (!r.exists) {
        c.notice(m, "Channel '" + chan + "' is not registered.");
        return;
      }
      auto acc = c.database().query(
          "SELECT COUNT(*) AS n FROM chanserv_access WHERE channel=?",
          {std::string(sv::irc_lower(chan))});
      auto ak = c.database().query(
          "SELECT COUNT(*) AS n FROM chanserv_akick WHERE channel=?",
          {std::string(sv::irc_lower(chan))});
      c.notice(m, "Channel:   " + chan);
      c.notice(m, "Founder:   " +
                      (r.founder.empty() ? std::string("(none)") : r.founder));
      c.notice(m, "Registered: " + ts_str(r.registered));
      c.notice(m, "Last used: " + ts_str(r.lastused));
      c.notice(m, "Default modes: " +
                      (r.modes.empty() ? std::string("+nt") : r.modes));
      c.notice(m, "Topic:     " +
                      (r.topic.empty() ? std::string("(none)") : r.topic));
      c.notice(
          m,
          "Access entries: " + (acc.empty() ? "0" : acc[0].as_string("n")) +
              ", AKICK entries: " + (ak.empty() ? "0" : ak[0].as_string("n")));
    });

    c.on_command("chanserv", "KICK", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: KICK <#channel> <nick> [reason]");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &nick = m.arg(1);
      std::string reason = m.arg(2);
      if (reason.empty())
        reason = "Kicked by ChanServ";
      irc::user *tu = c.net().by_nick(nick);
      if (!tu) {
        c.notice(m, "No such user '" + nick + "'.");
        return;
      }
      if (!has_access(c, m, chan, 500)) {
        c.notice(m, "You do not have permission to kick in '" + chan + "'.");
        return;
      }
      irc::message mk;
      mk.prefix = m.service->uid;
      mk.command = "KICK";
      mk.params.push_back(std::string(chan));
      mk.params.push_back(tu->uid);
      mk.params.push_back(reason);
      c.deliver(mk);
    });

    c.on_command("chanserv", "BAN", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: BAN <#channel> <mask> [reason]");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &mask = m.arg(1);
      if (!has_access(c, m, chan, 400)) {
        c.notice(m, "You do not have permission to ban in '" + chan + "'.");
        return;
      }
      send_chan_mode(c, c.service_uid("ChanServ"), chan, "+b", mask);
      c.notice(m, "Banned " + mask + " on " + chan + ".");
    });

    c.on_command("chanserv", "UNBAN", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: UNBAN <#channel> <mask>");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &mask = m.arg(1);
      if (!has_access(c, m, chan, 400)) {
        c.notice(m, "You do not have permission to change bans in '" + chan +
                        "'.");
        return;
      }
      send_chan_mode(c, c.service_uid("ChanServ"), chan, "-b", mask);
      c.notice(m, "Unbanned " + mask + " on " + chan + ".");
    });

    c.on_command("chanserv", "TOPIC", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: TOPIC <#channel> <new topic>");
        return;
      }
      if (!has_access(c, m, m.arg(0), 450)) {
        c.notice(m, "You do not have permission to change the topic on '" +
                        m.arg(0) + "'.");
        return;
      }
      send_topic(c, m, m.arg(0), m.join(1));
    });

    c.on_command("chanserv", "MODE", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: MODE <#channel> <modes> [params]");
        return;
      }
      if (!has_access(c, m, m.arg(0), 300)) {
        c.notice(m, "You do not have permission to change modes on '" +
                        m.arg(0) + "'.");
        return;
      }
      send_chan_mode(c, c.service_uid("ChanServ"), m.arg(0), m.arg(1),
                     m.join(2));
    });

    c.on_command("chanserv", "ACCESS", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: ACCESS <#channel> LIST | ADD <who> <role|level> | "
                    "DEL <who>");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &op = m.arg(1);
      std::string who = m.arg(2);
      if (!has_access(c, m, chan, 100)) {
        c.notice(m, "You do not have access to '" + chan + "'.");
        return;
      }
      if (sv::equals_ci(op, "ADD") || sv::equals_ci(op, "DEL")) {
        if (who.empty()) {
          c.notice(m, "Usage: ACCESS <#channel> " + op + " <who> [role|level]");
          return;
        }
        // Granting/revoking access needs 500+, and a user may never modify an
        // entry at their own level or above.
        int const caller = caller_level(c, m, chan);
        if (caller < 500) {
          c.notice(m, "You do not have permission to modify the access list "
                      "for '" +
                          chan + "'.");
          return;
        }
        int const existing = channel_access_level(c, chan, who);
        if (existing >= caller) {
          c.notice(m, "You cannot change access for a user at or above your "
                      "own level.");
          return;
        }
        // Never let anyone but the founder touch the founder's row.
        std::string const founder = channel_founder(c, chan);
        if (!founder.empty() && fold(who) == founder && caller < 600) {
          c.notice(m, "Only the channel founder can change access for the "
                      "founder.");
          return;
        }
      }
      if (sv::equals_ci(op, "ADD")) {
        int lv = 100;
        std::string const role = m.arg(3);
        if (!role.empty()) {
          int const rr = role_level(role);
          lv = rr >= 0 ? rr
                       : static_cast<int>(sv::parse_or(role, std::int64_t(-1)));
          if (lv < 0 || lv > 600) {
            c.notice(m, "Invalid level/role '" + role +
                            "'. Use a number 1-600 or a role name (VOICE, "
                            "HOP, OP, PROTECT, OWNER).");
            return;
          }
        }
        c.database().run("INSERT OR REPLACE INTO chanserv_access (channel, "
                         "who, level) VALUES (?,?,?)",
                         {std::string(sv::irc_lower(chan)), fold(who), lv});
        c.notice(m, "Access for " + who + " on " + chan + " set to " +
                        level_role(lv) + " (level " + std::to_string(lv) +
                        ").");
      } else if (sv::equals_ci(op, "DEL")) {
        c.database().run(
            "DELETE FROM chanserv_access WHERE channel=? AND who=?",
            {std::string(sv::irc_lower(chan)), fold(who)});
        c.notice(m, "Access removed for " + who + " on " + chan + ".");
      } else {
        auto rows = c.database().query("SELECT who, level FROM chanserv_access "
                                       "WHERE channel=? ORDER BY level DESC",
                                       {std::string(sv::irc_lower(chan))});
        std::string const founder = channel_founder(c, chan);
        c.notice(m, "Access list for " + chan +
                        (founder.empty()
                             ? std::string()
                             : std::string(" (founder ") + founder + ")"));
        if (rows.empty())
          c.notice(m, "  (no access entries)");
        for (auto &row : rows) {
          int const lv = static_cast<int>(row.as_int("level", 0));
          c.notice(m, "  " + row.as_string("who") + " -> " + level_role(lv) +
                          " (" + row.as_string("level") + ")");
        }
      }
    });

    c.on_command("chanserv", "SET", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: SET <#channel> <option> [value]");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &opt = m.arg(1);
      std::string val = m.arg(2);

      // Channel-registration properties are founder-only; mode features just
      // need PROTECT+ (400) since they mirror operator channel commands.
      bool const props =
          sv::equals_ci(opt, "PASSWORD") || sv::equals_ci(opt, "FOUNDER");
      if (!has_access(c, m, chan, props ? 600 : 400)) {
        c.notice(m, props ? "Only the founder can change settings for '" +
                                chan + "'."
                          : "You do not have permission to set modes on '" +
                                chan + "'.");
        return;
      }

      if (sv::equals_ci(opt, "PASSWORD")) {
        if (val.empty()) {
          c.notice(m, "Usage: SET <#channel> PASSWORD <password>");
          return;
        }
        c.database().run("UPDATE chanserv SET password=? WHERE name=?",
                         {val, std::string(sv::irc_lower(chan))});
        c.notice(m, "Channel password updated.");
        return;
      }
      if (sv::equals_ci(opt, "FOUNDER")) {
        if (val.empty()) {
          c.notice(m, "Usage: SET <#channel> FOUNDER <nick>");
          return;
        }
        c.database().run("UPDATE chanserv SET founder=? WHERE name=?",
                         {fold(val), std::string(sv::irc_lower(chan))});
        c.notice(m, "Founder updated.");
        return;
      }

      // ---- named channel-mode features (the server's CHANMODES) ----
      bool off = !opt.empty() && opt[0] == '-';
      std::string base = off ? std::string(opt.substr(1)) : opt;
      if (!off && base.size() > 2 && base.compare(0, 2, "NO") == 0) {
        off = true;
        base = base.substr(2);
      }
      feature const *f = feature_lookup(base);
      if (!f) {
        c.notice(m, "Unknown SET option for " + chan +
                        ". Use HELP SET for a "
                        "list.");
        return;
      }
      std::string const modes = std::string(off ? "-" : "+") + f->mode;
      std::string param;
      if (!off && f->takes_value) {
        if (val.empty()) {
          c.notice(m, "Usage: SET <#channel> " + base + " <value>");
          return;
        }
        param = val;
      }
      send_chan_mode(c, c.service_uid("ChanServ"), chan, modes, param);
      c.notice(m, std::string(off ? "Disabled " : "Enabled ") + base + " on " +
                      chan +
                      (param.empty() ? std::string()
                                     : std::string(" (" + param + ")")) +
                      ".");
    });

    c.on_command("chanserv", "DROP", [](ctx &c, cmsg const &m) {
      std::string const &chan = m.arg(0);
      if (chan.empty() || !sv::valid_chan(chan)) {
        c.notice(m, "Usage: DROP <#channel>");
        return;
      }
      if (!has_access(c, m, chan, 600)) {
        c.notice(m, "You do not have founder access to drop this channel.");
        return;
      }
      std::string const key = sv::irc_lower(chan);
      c.database().run("DELETE FROM chanserv WHERE name=?", {key});
      c.database().run("DELETE FROM chanserv_access WHERE channel=?", {key});
      c.database().run("DELETE FROM chanserv_akick WHERE channel=?", {key});
      c.database().run("DELETE FROM pending_chan WHERE name=?", {key});
      if (c.net().find_channel(chan) != nullptr)
        send_chan_mode(c, c.service_uid("ChanServ"), chan, "-r");
      c.notice(m, "Channel '" + chan + "' dropped.");
    });

    // Enforce channel AKICK lists whenever anyone (including a service bot)
    // joins a channel.
    c.on_user_join = [&c](irc::user const &u, irc::channel &ch) {
      enforce_akick(c, u, ch);
    };
  }

} // namespace svc::core