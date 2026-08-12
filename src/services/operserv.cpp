// InspiServices - OperServ: operator administration.
//
// Authorisation model: a command requires a *privilege*. Real IRC operators
// always have the full "admin" privilege; other users get privileges by being
// placed in a group (OperServ GROUP). The built-in privilege set:
//   admin        everything (implied by being an IRC oper or in an admin group)
//   status       STATUS, LINKINFO
//   mode         MODE
//   globops      GLOBOPS
//   kill         KILL
//   reburst      REBURST
//   approve      REGISTER (nick) APPROVE/REJECT/LIST
//   chan         REGISTER CHAN ...
//   autoapprove  AUTOAPPROVE
//   password     PASSWORD RESET
//   group        GROUP management
#include "services/irc/protocol.h"
#include "services/services/core.h"
#include "services/services/modules.h"
#include "services/services/syntax.h"
#include "services/util/crypto.h"
#include "services/util/log.h"
#include "services/util/util.h"

#include <ctime>
#include <set>

namespace svc::core {

  namespace {

    std::string global_get(ctx &c, std::string const &key,
                           std::string const &def = {}) {
      auto g = c.database().query(
          "SELECT value FROM global WHERE key=? LIMIT 1", {key});
      return g.empty() ? def : g[0].as_string("value");
    }

    bool auto_approve_state(ctx &c, std::string_view scope = "NICK") {
      std::string const key = scope == "CHAN" ? "auto_approve_chan"
                                              : "auto_approve";
      return global_get(c, key) == "1";
    }

    void set_auto_approve(ctx &c, bool on, std::string_view scope = "NICK") {
      std::string const key = scope == "CHAN" ? "auto_approve_chan"
                                              : "auto_approve";
      c.database().run("INSERT OR REPLACE INTO global (key, value) "
                       "VALUES (?, ?)",
                       {key, on ? "1" : "0"});
    }

    // All privileges the caller currently has, aggregated from IRC oper status
    // plus every group the caller's account/nick belongs to.
    std::set<std::string> effective_privs(ctx &c, std::string_view uid) {
      std::set<std::string> out;
      irc::user *u = c.net().find_user(uid);
      if (u && (u->mode.find_first_of("Oo") != std::string::npos ||
                !u->opertype.empty()))
        out.insert("admin");
      if (!u)
        return out;

      std::vector<std::string> who;
      if (!u->account.empty())
        who.push_back(fold(u->account));
      if (!u->nick.empty())
        who.push_back(fold(u->nick));
      for (auto const &w : who) {
        auto rows = c.database().query(
            "SELECT grp FROM oper_group_users WHERE who=? LIMIT 50", {w});
        for (auto const &r : rows) {
          auto g = c.database().query(
              "SELECT privileges FROM oper_groups WHERE name=? LIMIT 1",
              {r.as_string("grp")});
          if (g.empty())
            continue;
          for (auto const &p : sv::split(g[0].as_string("privileges"), ',')) {
            std::string const t = sv::trim(p);
            if (!t.empty())
              out.insert(t);
          }
        }
      }
      return out;
    }

    bool req_priv(ctx &c, cmsg const &m, std::string_view priv) {
      auto const &privs = effective_privs(c, m.sender);
      if (privs.count("admin") || privs.count(std::string(priv)))
        return true;
      c.notice(m, "You do not have permission to use this command.");
      return false;
    }

    // Legacy gate used by a couple of truly administrative subcommands.
    bool req_oper(ctx &c, cmsg const &m) { return req_priv(c, m, "group"); }

    const char *state_name(irc::link_state st) {
      switch (st) {
      case irc::link_state::idle:
        return "idle";
      case irc::link_state::connecting:
        return "connecting";
      case irc::link_state::nego:
        return "negotiating";
      case irc::link_state::waiting:
        return "waiting";
      case irc::link_state::burst:
        return "bursting";
      case irc::link_state::linked:
        return "linked";
      case irc::link_state::dying:
        return "dying";
      }
      return "unknown";
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

    // ---- pending registration queue ----

    // Approves or rejects a pending nickname registration.
    void approve_nick_pending(ctx &c, cmsg const &m, std::string const &nick,
                              bool approve) {
      auto rows =
          c.database().query("SELECT name, password, email FROM pending_reg "
                             "WHERE name=? AND approved=0 LIMIT 1",
                             {nick});
      if (rows.empty()) {
        c.notice(m, "No pending registration for '" + nick + "'.");
        return;
      }
      std::string const password = rows[0].as_string("password");
      std::string const email = rows[0].as_string("email");
      std::int64_t const now = svc::irc::now();

      c.database().run(
          "UPDATE pending_reg SET approved=?, approved_by=? WHERE name=?",
          {approve ? 1 : -1, std::string(m.sender), nick});

      if (approve) {
        c.database().run("INSERT INTO nickserv (name, password, salt, email, "
                         "registered, lastseen, account) "
                         "VALUES (?, ?, '', ?, ?, ?, ?)",
                         {nick, password, email, now, now, nick});
        c.notice(m, "Registration for '" + nick + "' approved.");
      } else {
        c.notice(m, "Registration for '" + nick + "' rejected.");
      }
    }

    // Approves or rejects a pending channel registration.
    void approve_chan_pending(ctx &c, cmsg const &m, std::string const &chan,
                              bool approve) {
      std::string const key = sv::irc_lower(chan);
      auto rows = c.database().query(
          "SELECT founder FROM pending_chan WHERE name=? AND approved=0 LIMIT "
          "1",
          {key});
      if (rows.empty()) {
        c.notice(m, "No pending channel registration for '" + chan + "'.");
        return;
      }
      std::string const founder = rows[0].as_string("founder");
      c.database().run("UPDATE pending_chan SET approved=?, approved_by=? "
                       "WHERE name=?",
                       {approve ? 1 : -1, std::string(m.sender), key});
      if (approve) {
        activate_channel(c, chan, founder);
        c.notice(m, "Registration for channel '" + chan + "' approved.");
      } else {
        c.notice(m, "Registration for channel '" + chan + "' rejected.");
      }
    }

    // Approves every registration currently waiting in `scope` ("NICK" or
    // "CHAN"). Used when auto-approve is switched on so queued requests do not
    // stay stuck.
    std::size_t approve_all_pending(ctx &c, cmsg const &m,
                                    std::string_view scope) {
      std::size_t n = 0;
      if (scope == "CHAN") {
        auto rows =
            c.database().query("SELECT name FROM pending_chan WHERE "
                               "approved=0 ORDER BY requested ASC");
        for (auto const &r : rows) {
          approve_chan_pending(c, m, r.as_string("name"), true);
          ++n;
        }
      } else {
        auto rows =
            c.database().query("SELECT name FROM pending_reg WHERE "
                               "approved=0 ORDER BY requested ASC");
        for (auto const &r : rows) {
          approve_nick_pending(c, m, r.as_string("name"), true);
          ++n;
        }
      }
      return n;
    }

    void list_nick_pending(ctx &c, cmsg const &m) {
      auto rows = c.database().query(
          "SELECT name, email, requested FROM pending_reg WHERE approved=0 "
          "ORDER BY requested ASC LIMIT 100");
      if (rows.empty()) {
        c.notice(m, "No pending registrations.");
        return;
      }
      c.notice(m, "Pending registrations:");
      for (auto const &r : rows) {
        std::string const em = r.as_string("email");
        c.notice(m, "  " + r.as_string("name") +
                        " (email: " + (em.empty() ? "-" : em) + ", at " +
                        ts_str(r.as_int("requested")) + ")");
      }
    }

    void list_chan_pending(ctx &c, cmsg const &m) {
      auto rows = c.database().query("SELECT name, founder, requested FROM "
                                     "pending_chan WHERE approved=0 ORDER BY "
                                     "requested ASC LIMIT 100");
      if (rows.empty()) {
        c.notice(m, "No pending channel registrations.");
        return;
      }
      c.notice(m, "Pending channel registrations:");
      for (auto const &r : rows)
        c.notice(m, "  " + r.as_string("name") + " (by " +
                        r.as_string("founder") + ", at " +
                        ts_str(r.as_int("requested")) + ")");
    }

    // Plural display label for an auto-approve scope.
    std::string scope_label(std::string_view scope) {
      return scope == "CHAN" ? std::string("channels")
                             : std::string("registrations");
    }
    // Singular "item" label for an auto-approve sweep.
    std::string scope_item(std::string_view scope) {
      return scope == "CHAN" ? std::string("channel")
                             : std::string("nickname");
    }

  } // namespace

  void install_operserv(ctx &c) {
    c.add_help("operserv", "STATUS",
               "Usage: STATUS\n"
               "Shows registered nick/channel/access/group counts. Requires "
               "the 'status' privilege.");
    c.add_help("operserv", "LINKINFO",
               "Usage: LINKINFO (or SPANTREE)\n"
               "Lists spanning-tree sessions and remote server details.");
    c.add_help("operserv", "MODE",
               "Usage: MODE <target> <modes>\n"
               "Sets modes on a user or channel.");
    c.add_help("operserv", "GLOBOPS",
               "Usage: GLOBOPS <text>\n"
               "Broadcasts a notice to every operator.");
    c.add_help("operserv", "KILL",
               "Usage: KILL <nick> [reason]\n"
               "Disconnects a user from the network.");
    c.add_help("operserv", "REBURST",
               "Usage: REBURST\n"
               "Re-sends the services burst to all linked servers.");
    c.add_help("operserv", "REGISTER",
               "Usage: REGISTER APPROVE <nick> | REJECT <nick> | LIST | CHAN "
               "APPROVE <#chan> | CHAN REJECT <#chan> | CHAN LIST\n"
               "       REGISTER AUTOAPPROVE [on|off]\n"
               "Manages pending nickname and channel registrations. "
               "CHAN APPROVE <#chan> approves a channel request, CHAN "
               "REJECT/CHAN LIST manage the rest of the queue.");
    c.add_help("operserv", "AUTOAPPROVE",
               "Usage: AUTOAPPROVE [NICK|CHAN] [on|off]\n"
               "Shows or toggles automatic approval of new registrations.");
    c.add_help("operserv", "PASSWORD",
               "Usage: PASSWORD RESET <nick> <newpassword>\n"
               "Resets a registered nick's password.");
    c.add_help("operserv", "GROUP",
               "Usage: GROUP LIST | ADD <name> | DEL <name> | VIEW <name>\n"
               "       GROUP PRIV <name> ADD|DEL <priv>\n"
               "       GROUP USER <name> ADD|DEL <who>\n"
               "Manages privilege groups that grant non-oper users command "
               "access. Groups define named permissions (status, mode, kill, "
               "reburst, approve, chan, autoapprove, password, group).");

    c.on_command("operserv", "STATUS", [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "status"))
        return;
      auto const nicks =
          c.database().query("SELECT COUNT(*) AS n FROM nickserv");
      auto const chans =
          c.database().query("SELECT COUNT(*) AS n FROM chanserv");
      auto const users =
          c.database().query("SELECT COUNT(*) AS n FROM chanserv_access");
      auto const grps =
          c.database().query("SELECT COUNT(*) AS n FROM oper_groups");
      std::int64_t nu = nicks.empty() ? 0 : nicks[0].as_int("n");
      std::int64_t cu = chans.empty() ? 0 : chans[0].as_int("n");
      std::int64_t uu = users.empty() ? 0 : users[0].as_int("n");
      std::int64_t gu = grps.empty() ? 0 : grps[0].as_int("n");
      c.notice(m, "InspiServices status:");
      c.notice(m, "  registered nicks:   " + std::to_string(nu));
      c.notice(m, "  registered chans:   " + std::to_string(cu));
      c.notice(m, "  access list rows:   " + std::to_string(uu));
      c.notice(m, "  privilege groups:   " + std::to_string(gu));
    });

    // ---- spanning tree session / link info ----
    auto linkinfo = [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "status"))
        return;
      auto const &hcfg = c.the_hub().cfg();
      c.notice(m, std::string("Server: ") + hcfg.server_name + " (" +
                      hcfg.server_sid + ") " + hcfg.server_desc);
      c.notice(m, "Spanning tree sessions:");
      auto const &links = c.the_hub().links();
      if (links.empty()) {
        c.notice(m, "  (none)");
        return;
      }
      for (auto *l : links) {
        auto const &lc = l->config();
        std::string const port = lc.port.empty() ? "?" : lc.port;
        std::string const dir = l->is_inbound() ? "inbound" : "outbound";
        std::string const tls = lc.send_tls ? "tls" : "plain";
        std::string const remote = l->remote_name().empty()
                                       ? "(no remote name yet)"
                                       : l->remote_name();
        std::string const sid = l->remote_sid().empty()
                                    ? std::string("------")
                                    : l->remote_sid();
        c.notice(m, sv::fmt("  {} ({}) [{}] {} {}://{}:{} {}", remote, sid,
                            state_name(l->state()), dir, lc.host.empty() ? "?"
                                                                          : lc.host,
                            port, tls,
                            l->linked() ? "" : "(not fully linked)"));
      }
    };
    c.on_command("operserv", "LINKINFO", linkinfo);
    c.on_command("operserv", "SPANTREE", linkinfo);

    c.on_command("operserv", "MODE", [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "mode"))
        return;
      if (m.argc() < 2) {
        c.notice(m, "Usage: MODE <target> <modes>");
        return;
      }
      irc::message mk;
      mk.prefix = m.service->uid;
      mk.command = "MODE";
      mk.params.push_back(m.arg(0));
      mk.params.push_back(m.arg(1));
      c.deliver(mk);
    });

    c.on_command("operserv", "GLOBOPS", [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "globops"))
        return;
      std::string text = m.join(0);
      if (text.empty()) {
        c.notice(m, "Usage: GLOBOPS <text>");
        return;
      }
      irc::message mk;
      mk.prefix = m.service->uid;
      mk.command = "GLOBOPS";
      mk.params.push_back(text);
      c.deliver(mk);
    });

    c.on_command("operserv", "KILL", [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "kill"))
        return;
      if (m.argc() < 1) {
        c.notice(m, "Usage: KILL <nick> [reason]");
        return;
      }
      irc::user *tu = c.net().by_nick(m.arg(0));
      if (!tu) {
        c.notice(m, "No such user '" + m.arg(0) + "'.");
        return;
      }
      std::string reason = m.arg(1);
      if (reason.empty())
        reason = "Killed by OperServ";
      irc::message mk;
      mk.prefix = m.service->uid;
      mk.command = "KILL";
      mk.params.push_back(tu->uid);
      mk.params.push_back(reason);
      c.deliver(mk);
      c.notice(m, "Killed " + tu->nick + ".");
    });

    c.on_command("operserv", "REBURST", [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "reburst"))
        return;
      for (auto *l : c.the_hub().links())
        l->send_burst();
      c.notice(m, "Rebursting all links.");
    });

    // ---- pending registration management ----
    c.on_command("operserv", "REGISTER", [](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: REGISTER APPROVE <nick> | REJECT <nick> | LIST | "
                     "AUTOAPPROVE [on|off] | CHAN APPROVE <#chan> | CHAN "
                     "REJECT <#chan> | CHAN LIST");
        return;
      }
      std::string const &sub = m.arg(0);

      if (sv::equals_ci(sub, "CHAN")) {
        if (!req_priv(c, m, "chan"))
          return;
        if (m.argc() < 2) {
          c.notice(m, "Usage: REGISTER CHAN APPROVE <#chan> | CHAN REJECT "
                      "<#chan> | CHAN LIST");
          return;
        }
        std::string const &s2 = m.arg(1);
        if (sv::equals_ci(s2, "APPROVE") || sv::equals_ci(s2, "REJECT")) {
          if (m.argc() < 3) {
            c.notice(m, "Usage: REGISTER CHAN APPROVE <#chan> | CHAN REJECT "
                        "<#chan>");
            return;
          }
          approve_chan_pending(c, m, m.arg(2), sv::equals_ci(s2, "APPROVE"));
        } else if (sv::equals_ci(s2, "LIST")) {
          list_chan_pending(c, m);
        } else {
          c.notice(m, "Unknown REGISTER CHAN subcommand.");
        }
        return;
      }

      if (sv::equals_ci(sub, "APPROVE")) {
        if (!req_priv(c, m, "approve"))
          return;
        if (m.argc() < 2) {
          c.notice(m, "Usage: REGISTER APPROVE <nick>");
          return;
        }
        approve_nick_pending(c, m, m.arg(1), true);
        return;
      }

      if (sv::equals_ci(sub, "REJECT")) {
        if (!req_priv(c, m, "approve"))
          return;
        if (m.argc() < 2) {
          c.notice(m, "Usage: REGISTER REJECT <nick>");
          return;
        }
        approve_nick_pending(c, m, m.arg(1), false);
        return;
      }

      if (sv::equals_ci(sub, "LIST")) {
        if (!req_priv(c, m, "approve"))
          return;
        // "REGISTER LIST CHAN" is accepted as well as "REGISTER CHAN LIST".
        if (sv::equals_ci(m.arg(1), "CHAN")) {
          list_chan_pending(c, m);
          return;
        }
        list_nick_pending(c, m);
        return;
      }

      if (sv::equals_ci(sub, "AUTOAPPROVE")) {
        if (!req_priv(c, m, "autoapprove"))
          return;
        bool on = true;
        if (m.argc() >= 2) {
          std::string const &val = m.arg(1);
          if (sv::equals_ci(val, "on") || val == "1")
            on = true;
          else if (sv::equals_ci(val, "off") || val == "0")
            on = false;
          else {
            c.notice(m, "Usage: REGISTER AUTOAPPROVE [on|off]");
            return;
          }
        }
        set_auto_approve(c, on, "NICK");
        c.notice(m, std::string("Nickname auto-approve ") +
                        (on ? "enabled." : "disabled."));
        if (on) {
          std::size_t const n = approve_all_pending(c, m, "NICK");
          c.notice(m, "Approved " + std::to_string(n) +
                          " pending nickname registration(s).");
        }
        return;
      }

      c.notice(m, "Unknown REGISTER subcommand. Use APPROVE, REJECT, LIST, "
                  "AUTOAPPROVE, or CHAN.");
    });

    // Simple toggle used directly by operators.
    c.on_command("operserv", "AUTOAPPROVE", [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "autoapprove"))
        return;
      std::string scope = "NICK";
      std::size_t idx = 0;
      if (m.argc() >= 1 &&
          (sv::equals_ci(m.arg(0), "NICK") ||
           sv::equals_ci(m.arg(0), "CHAN"))) {
        scope = sv::equals_ci(m.arg(0), "CHAN") ? "CHAN" : "NICK";
        idx = 1;
      }
      if (m.argc() <= idx) {
        bool const on = auto_approve_state(c, scope);
        c.notice(m, "Auto-approve of " + scope_label(scope) + " is " +
                        (on ? "enabled." : "disabled."));
        return;
      }
      std::string const &val = m.arg(idx);
      if (sv::equals_ci(val, "on") || val == "1") {
        set_auto_approve(c, true, scope);
        c.notice(m, "Auto-approve of " + scope_label(scope) + " enabled.");
        std::size_t const n = approve_all_pending(c, m, scope);
        c.notice(m, "Approved " + std::to_string(n) + " pending " +
                        scope_item(scope) + " registration(s).");
      } else if (sv::equals_ci(val, "off") || val == "0") {
        set_auto_approve(c, false, scope);
        c.notice(m, "Auto-approve of " + scope_label(scope) + " disabled.");
      } else {
        c.notice(m, "Usage: AUTOAPPROVE [NICK|CHAN] [on|off]");
      }
    });

    // ---- Password reset (oper override) ----
    c.on_command("operserv", "PASSWORD", [](ctx &c, cmsg const &m) {
      if (!req_priv(c, m, "password"))
        return;
      if (m.argc() < 2 || !sv::equals_ci(m.arg(0), "RESET")) {
        c.notice(m, "Usage: PASSWORD RESET <nick> <newpassword>");
        return;
      }
      std::string const nick = m.arg(1);
      std::string const newpass = m.arg(2);
      if (newpass.size() < 6) {
        c.notice(m, "Passwords must be at least 6 characters long.");
        return;
      }
      auto rows =
          c.database().query("SELECT name FROM nickserv WHERE name=? LIMIT 1",
                             {std::string(fold(nick))});
      if (rows.empty()) {
        c.notice(m, "No such registered nick '" + nick + "'.");
        return;
      }
      std::string const folded = rows[0].as_string("name");
      auto hash_pw = [](std::string_view pw) -> std::string {
        std::string const salt = crypto::random_hex(16);
        return "1:" + salt + ":" +
               crypto::base64_encode(crypto::pbkdf2_sha256(pw, salt, 64000));
      };
      c.database().run("UPDATE nickserv SET password=?, salt='' WHERE name=?",
                       {hash_pw(newpass), folded});
      c.notice(m, "Password for '" + nick + "' reset.");
      log::info("os", "{} reset password for {}", m.sender, nick);
    });

    // ---- Privilege groups ----
    c.on_command("operserv", "GROUP", [](ctx &c, cmsg const &m) {
      if (!req_oper(c, m))
        return;
      std::string const &sub = m.arg(0);
      if (sv::equals_ci(sub, "LIST")) {
        auto rows = c.database().query(
            "SELECT g.name, g.privileges, COUNT(u.who) AS members FROM "
            "oper_groups g LEFT JOIN oper_group_users u ON u.grp = g.name "
            "GROUP BY g.name ORDER BY g.name");
        c.notice(m, "Privilege groups:");
        bool any = false;
        for (auto const &r : rows) {
          any = true;
          c.notice(m, "  " + r.as_string("name") + " [" +
                          r.as_string("privileges") + "] (" +
                          r.as_string("members") + " member(s))");
        }
        if (!any)
          c.notice(m, "  (no groups configured)");
      } else if (sv::equals_ci(sub, "ADD")) {
        if (m.argc() < 2) {
          c.notice(m, "Usage: GROUP ADD <name>");
          return;
        }
        c.database().run("INSERT OR IGNORE INTO oper_groups (name, "
                         "privileges) VALUES (?, '')",
                         {m.arg(1)});
        c.notice(m, "Group '" + m.arg(1) + "' created.");
      } else if (sv::equals_ci(sub, "DEL")) {
        if (m.argc() < 2) {
          c.notice(m, "Usage: GROUP DEL <name>");
          return;
        }
        c.database().run("DELETE FROM oper_groups WHERE name=?", {m.arg(1)});
        c.database().run("DELETE FROM oper_group_users WHERE grp=?",
                         {m.arg(1)});
        c.notice(m, "Group '" + m.arg(1) + "' deleted.");
      } else if (sv::equals_ci(sub, "VIEW")) {
        if (m.argc() < 2) {
          c.notice(m, "Usage: GROUP VIEW <name>");
          return;
        }
        auto g = c.database().query(
            "SELECT privileges FROM oper_groups WHERE name=? LIMIT 1",
            {m.arg(1)});
        if (g.empty()) {
          c.notice(m, "No such group '" + m.arg(1) + "'.");
          return;
        }
        std::string const privs = g[0].as_string("privileges");
        c.notice(m, "Group '" + m.arg(1) + "' privileges: [" +
                        (privs.empty() ? "none" : privs) + "]");
        auto mem = c.database().query(
            "SELECT who FROM oper_group_users WHERE grp=? ORDER BY who",
            {m.arg(1)});
        if (mem.empty())
          c.notice(m, "  (no members)");
        else {
          c.notice(m, "  Members:");
          for (auto const &r : mem)
            c.notice(m, "    " + r.as_string("who"));
        }
      } else if (sv::equals_ci(sub, "PRIV")) {
        if (m.argc() < 4) {
          c.notice(m, "Usage: GROUP PRIV <name> ADD|DEL <priv>");
          return;
        }
        std::string const &name = m.arg(1);
        std::string const &op = m.arg(2);
        std::string const &priv = m.arg(3);
        auto g = c.database().query(
            "SELECT privileges FROM oper_groups WHERE name=? LIMIT 1", {name});
        if (g.empty()) {
          c.notice(m, "No such group '" + name + "'.");
          return;
        }
        std::set<std::string> cur;
        for (auto const &p : sv::split(g[0].as_string("privileges"), ',')) {
          std::string const t = sv::trim(p);
          if (!t.empty())
            cur.insert(t);
        }
        if (sv::equals_ci(op, "ADD"))
          cur.insert(priv);
        else if (sv::equals_ci(op, "DEL"))
          cur.erase(priv);
        else {
          c.notice(m, "Usage: GROUP PRIV <name> ADD|DEL <priv>");
          return;
        }
        std::string joined;
        for (auto const &p : cur) {
          if (!joined.empty())
            joined.push_back(',');
          joined += p;
        }
        c.database().run("UPDATE oper_groups SET privileges=? WHERE name=?",
                         {joined, name});
        c.notice(m, "Group '" + name + "' privileges: [" +
                        (joined.empty() ? "none" : joined) + "]");
      } else if (sv::equals_ci(sub, "USER")) {
        if (m.argc() < 4) {
          c.notice(m, "Usage: GROUP USER <name> ADD|DEL <who>");
          return;
        }
        std::string const &name = m.arg(1);
        std::string const &op = m.arg(2);
        std::string const &who = m.arg(3);
        auto g = c.database().query(
            "SELECT name FROM oper_groups WHERE name=? LIMIT 1", {name});
        if (g.empty()) {
          c.notice(m, "No such group '" + name + "'.");
          return;
        }
        std::string const folded = fold(who);
        if (sv::equals_ci(op, "ADD"))
          c.database().run("INSERT OR IGNORE INTO oper_group_users (grp, who) "
                           "VALUES (?, ?)",
                           {name, folded});
        else if (sv::equals_ci(op, "DEL"))
          c.database().run(
              "DELETE FROM oper_group_users WHERE grp=? AND who=?",
              {name, folded});
        else {
          c.notice(m, "Usage: GROUP USER <name> ADD|DEL <who>");
          return;
        }
        c.notice(m, "Group '" + name + "' user '" + who + "' " +
                        (sv::equals_ci(op, "ADD") ? "added." : "removed."));
      } else {
        c.notice(m, "Unknown GROUP subcommand. Use LIST, ADD, DEL, VIEW, "
                    "PRIV, or USER.");
      }
    });
  }

} // namespace svc::core