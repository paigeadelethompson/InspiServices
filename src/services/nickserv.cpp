// AnswerServices - NickServ: nickname registration and identification.
#include "services/irc/protocol.h"
#include "services/services/core.h"
#include "services/services/modules.h"
#include "services/services/syntax.h"
#include "services/util/crypto.h"
#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::core {

  namespace {

    constexpr std::int64_t lastseen_interval = 7 * 86400; // week

    struct account {
      bool exists = false;
      std::string password;
      std::string email;
      std::string account;
      std::int64_t registered = 0;
      std::int64_t lastseen = 0;
    };

    // Password stored as "1:<salthex>:<base64(pbkdf2)>".
    std::string hash_pass(std::string_view password) {
      std::string const salt = crypto::random_hex(16);
      return std::string("1:") + salt + ":" +
             crypto::base64_encode(
                 crypto::pbkdf2_sha256(password, salt, 64000));
    }

    bool check_pass(std::string const &stored, std::string_view password) {
      if (stored.size() < 5 || stored[0] != '1')
        return false;
      std::size_t const colon = stored.find(':', 2);
      if (colon == std::string::npos || stored.size() < colon + 2)
        return false;
      std::string const salt = stored.substr(2, colon - 2);
      std::string const want = stored.substr(colon + 1);
      std::string const got =
          crypto::base64_encode(crypto::pbkdf2_sha256(password, salt, 64000));
      return crypto::timing_safe_equal(got, want);
    }

    account load_account(ctx &c, std::string_view folded) {
      account a;
      auto rows =
          c.database().query("SELECT password, email, account, registered, "
                             "lastseen FROM nickserv WHERE name=? LIMIT 1",
                             {std::string(folded)});
      if (rows.empty())
        return a;
      a.exists = true;
      a.password = rows[0].as_string("password");
      a.email = rows[0].as_string("email");
      a.account = rows[0].as_string("account");
      a.registered = rows[0].as_int("registered");
      a.lastseen = rows[0].as_int("lastseen");
      return a;
    }

    // Usermodes users may set for themselves via SET UMODES (everything except
    // operator/operational modes, which the ircd owns).
    char const *const k_allowed_umodes =
        "ixdcRBDTgSwNIWLzh"; // d,c,R,D,x,i,B,T,S,w,N,I,g,W,L,z,h

    // Applies a "+a-b..." usermode delta to `uid`, keeping the live network
    // model in sync with what we tell the ircd.
    void apply_umode(ctx &c, std::string_view uid, std::string_view delta) {
      std::string add, sub;
      bool plus = true;
      for (char ch : delta) {
        if (ch == '+')
          plus = true;
        else if (ch == '-')
          plus = false;
        else if (ch != ' ' && ch != ',')
          (plus ? add : sub) += ch;
      }
      if (add.empty() && sub.empty())
        return;
      if (irc::user *u = c.net().find_user(uid)) {
        std::string nm;
        for (char ch : u->mode)
          if (ch != ' ' && sub.find(ch) == std::string::npos &&
              add.find(ch) == std::string::npos)
            nm += ch;
        for (char ch : add)
          if (nm.find(ch) == std::string::npos)
            nm += ch;
        u->mode = nm;
      }
      irc::message m;
      m.prefix = c.service_uid("NickServ");
      m.command = "MODE";
      m.params.push_back(std::string(uid));
      m.params.push_back(std::string(delta));
      c.deliver(m);
      log::debug("ns", "umode {} applied to {}", delta, uid);
    }

    void set_account(ctx &c, std::string_view uid, std::string_view account) {
      irc::user *u = c.net().find_user(uid);
      if (!u)
        return;
      u->account = std::string(account);
      // METADATA must originate from our server, not the target user: a
      // prefix pointing at a user of another server makes the ircd flag the
      // line as a "Fake direction" violation and drop it.
      irc::message m;
      m.prefix = c.the_hub().cfg().server_sid;
      m.command = "METADATA";
      m.params.emplace_back(std::string(uid));
      m.params.emplace_back("accountname");
      m.params.emplace_back(account.empty() ? std::string("0")
                                            : std::string(account));
      c.deliver(m);
      // Mirror the ircd's c_registered drop of +r on logout. Keep the model
      // and the wire in sync with the same message.
      apply_umode(c, uid, account.empty() ? "-r" : "+r");
    }

    // Applies the umodes a nickname persists in SET UMODES after identifying.
    void apply_saved_umodes(ctx &c, std::string_view uid,
                            std::string_view who) {
      auto rows = c.database().query(
          "SELECT umodes FROM nickserv WHERE name=? OR account=? LIMIT 1",
          {std::string(who), std::string(who)});
      if (rows.empty())
        return;
      std::string const um = rows[0].as_string("umodes");
      if (!um.empty())
        apply_umode(c, uid, um);
    }

    // Friendly display of an int64 unix timestamp.
    std::string ts_display(std::int64_t ts) {
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

    // Account (folded) bound to a client certificate fingerprint, if any.
    bool cert_account(ctx &c, std::string_view certfp,
                      std::string &account) {
      auto rows = c.database().query(
          "SELECT n.account FROM nickserv_cert nc "
          "JOIN nickserv n ON n.name = nc.name "
          "WHERE nc.certfp = ? LIMIT 1",
          {std::string(certfp)});
      if (rows.empty())
        return false;
      account = rows[0].as_string("account");
      return true;
    }

  } // namespace

  void install_nickserv(ctx &c) {

    c.add_help("nickserv", "REGISTER",
               "Usage: REGISTER <password> [email]\n"
               "Registers the nickname you are currently using. The password "
               "must be at least 6 characters. If auto-approve is disabled an "
               "operator must approve the request first.");
    c.add_help("nickserv", "IDENTIFY",
               "Usage: IDENTIFY [nickname] <password>\n"
               "Logs into a registered nickname. Without a nickname your "
               "current nick is used. You can also authenticate automatically "
               "by binding a client certificate with CERT.");
    c.add_help("nickserv", "LOGOUT", "Usage: LOGOUT\n"
               "Ends your current login session.");
    c.add_help("nickserv", "STATUS",
               "Usage: STATUS [nickname]\n"
               "Shows whether a nickname is registered and whether you are "
               "currently identified to it.");
    c.add_help("nickserv", "SET",
               "Usage: SET PASSWORD <new>\n"
               "       SET EMAIL <new>\n"
               "       SET UMODES <modes>\n"
               "Updates your password, contact email, or the usermodes applied "
               "every time you identify (e.g. +i or +i-d; +r is set "
               "automatically). Identified users get +r.");
    c.add_help("nickserv", "GHOST",
               "Usage: GHOST <nick> [password]\n"
               "Forces a stale session using your nickname to disconnect, so "
               "you can take it over. The password must be supplied unless "
               "you are already identified to the account.");
    c.add_help("nickserv", "INFO",
               "Usage: INFO [nickname]\n"
               "Shows registration details (registered/last seen/email) for "
               "a nickname; defaults to your own.");
    c.add_help("nickserv", "DROP",
               "Usage: DROP [nickname]\n"
               "Deletes the registration for a nickname you own (operators "
               "may drop any registration).");
    c.add_help("nickserv", "CERT",
               "Usage: CERT ADD | CERT DEL | CERT LIST\n"
               "Binds a TLS client certificate to your registration. Once "
               "bound, connecting with that certificate identifies you "
               "automatically. Runs on a connection that has a certificate "
               "(e.g. via SASL/certfp).");

    c.on_command("nickserv", "CERT", [](ctx &c, cmsg const &m) {
      irc::user *u = c.net().find_user(m.sender);
      if (!u)
        return;
      std::string const &op = m.arg(0);
      bool const is_list = op.empty() || sv::equals_ci(op, "LIST");

      // All registered nicknames of the (possibly unlogged) caller's account.
      std::vector<std::string> mine;
      if (!u->account.empty()) {
        auto rows = c.database().query(
            "SELECT name, account FROM nickserv WHERE account=? ORDER BY name",
            {fold(u->account)});
        for (auto const &r : rows)
          mine.push_back(r.as_string("name"));
      }

      if (sv::equals_ci(op, "ADD")) {
        if (u->account.empty()) {
          c.notice(m, "You must be identified before binding a certificate.");
          return;
        }
        if (u->certfp.empty()) {
          c.notice(m, "Your current connection has no client certificate "
                      "(connect over TLS with a client cert, or via "
                      "SASL/certfp).");
          return;
        }
        std::string target;
        auto rows = c.database().query(
            "SELECT name FROM nickserv WHERE account=? AND name=? LIMIT 1",
            {fold(u->account), fold(u->nick)});
        if (rows.empty()) {
          if (mine.empty()) {
            c.notice(m, "No registered nickname found to bind the certificate "
                        "to.");
            return;
          }
          target = mine[0];
        } else
          target = rows[0].as_string("name");
        auto already = c.database().query(
            "SELECT name FROM nickserv_cert WHERE certfp=? LIMIT 1",
            {u->certfp});
        std::string const fp_mid =
            u->certfp.size() > 12 ? u->certfp.substr(0, 12) : u->certfp;
        if (!already.empty()) {
          c.notice(m, "That certificate (" + fp_mid +
                          "...) is already bound to '" +
                          already[0].as_string("name") + "'.");
          return;
        }
        c.database().run("INSERT OR IGNORE INTO nickserv_cert (name, certfp) "
                         "VALUES (?, ?)",
                         {target, u->certfp});
        c.notice(m, "Client certificate bound to '" + target +
                        "'. You will be identified automatically in future.");
        log::info("ns", "{} bound cert {} to {}", m.sender, u->certfp, target);
        return;
      }
      if (sv::equals_ci(op, "DEL")) {
        if (u->certfp.empty()) {
          c.notice(m, "Your current connection has no client certificate.");
          return;
        }
        c.database().run("DELETE FROM nickserv_cert WHERE certfp=?",
                         {u->certfp});
        c.notice(m, "Client certificate unbound.");
        return;
      }
      if (!is_list) {
        c.notice(m, "Usage: CERT ADD | CERT DEL | CERT LIST");
        return;
      }
      if (mine.empty()) {
        c.notice(m, "You have no registered certificates.");
        return;
      }
      c.notice(m, "Certificate(s) bound:");
      for (auto const &nm : mine) {
        auto rows = c.database().query(
            "SELECT certfp FROM nickserv_cert WHERE name=? ORDER BY certfp",
            {nm});
        for (auto const &r : rows) {
          std::string const fp = r.as_string("certfp");
          c.notice(m, "  " + nm + "  " +
                          (fp.size() > 12 ? fp.substr(0, 12) : fp) + "...");
        }
      }
    });

    c.on_command("nickserv", "REGISTER", [](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: REGISTER <password> [email]");
        return;
      }
      std::string const &password = m.arg(0);
      if (password.size() < 6) {
        c.notice(m, "Passwords must be at least 6 characters long.");
        return;
      }
      irc::user *u = c.net().find_user(m.sender);
      if (!u)
        return;
      std::string const folded = fold(u->nick);
      if (folded.empty())
        return;
      if (!sv::valid_nick(u->nick)) {
        c.notice(m, "You cannot register this nickname.");
        return;
      }
      if (load_account(c, folded).exists) {
        c.notice(m, "The nickname '" + u->nick + "' is already registered.");
        return;
      }
      auto pend = c.database().query(
          "SELECT name FROM pending_reg WHERE name=? AND approved=0 LIMIT 1",
          {folded});
      if (!pend.empty()) {
        c.notice(m, "Registration for '" + u->nick +
                        "' is already pending operator approval.");
        return;
      }
      std::string email = m.argc() > 1 ? m.arg(1) : std::string();
      if (!email.empty() && (email.find('@') == std::string::npos ||
                             email.find('.') == std::string::npos)) {
        c.notice(m, "Invalid email address.");
        return;
      }
      std::int64_t const now = svc::irc::now();
      std::string const hashed = hash_pass(password);

      // Check if auto-approve is enabled.
      bool auto_approve = false;
      auto g = c.database().query(
          "SELECT value FROM global WHERE key='auto_approve' LIMIT 1");
      if (!g.empty())
        auto_approve = g[0].as_string("value") == "1";

      if (auto_approve) {
        c.database().run("INSERT INTO nickserv (name, password, salt, email, "
                         "registered, lastseen, account) "
                         "VALUES (?, ?, '', ?, ?, ?, ?)",
                         {std::string(folded), hashed, email, now, now,
                          std::string(u->nick)});
        set_account(c, m.sender, u->nick);
        c.notice(m, "Nickname '" + u->nick +
                        "' registered. Identify with "
                        "/msg NickServ IDENTIFY " +
                        u->nick + " <password>");
        log::info("ns", "{} registered as {} (auto)", m.sender, u->nick);
      } else {
        c.database().run(
            "INSERT INTO pending_reg (name, password, email, requested) "
            "VALUES (?, ?, ?, ?)",
            {std::string(folded), hashed, email, now});
        c.notice(m, "Registration for '" + u->nick +
                        "' submitted and is pending "
                        "operator approval.");
        log::info("ns", "{} pending registration for {}", m.sender, u->nick);
      }
    });

    c.on_command("nickserv", "IDENTIFY", [](ctx &c, cmsg const &m) {
      std::string nick, password;
      if (m.argc() >= 2) {
        nick = m.arg(0);
        password = m.arg(1);
      } else if (m.argc() == 1) {
        password = m.arg(0);
        irc::user *u = c.net().find_user(m.sender);
        nick = u ? u->nick : std::string();
      } else {
        c.notice(m, "Usage: IDENTIFY <nick> <password>");
        return;
      }
      std::string const folded = fold(nick);
      account a = load_account(c, folded);
      if (!a.exists) {
        c.notice(m, "Nickname '" + nick + "' is not registered.");
        return;
      }
      // Already identified to this account? Nothing to do, and do not require
      // the password again - the ircd tells us who is logged in via the
      // accountname metadata.
      std::string const want = a.account.empty() ? folded : a.account;
      irc::user *self = c.net().find_user(m.sender);
      if (self && !self->account.empty() && fold(self->account) == want) {
        c.notice(m, "You are already identified as '" + self->account + "'.");
        return;
      }
      if (!check_pass(a.password, password)) {
        c.notice(m, "Incorrect password for '" + nick + "'.");
        return;
      }
      std::int64_t const now = svc::irc::now();
      if (a.lastseen + lastseen_interval <= now)
        c.database().run("UPDATE nickserv SET lastseen=? WHERE name=?",
                         {now, std::string(folded)});
      set_account(c, m.sender, a.account.empty() ? nick : a.account);
      apply_saved_umodes(c, m.sender, folded);
      c.notice(m, "You are now identified as '" +
                      (a.account.empty() ? nick : a.account) + "'.");
      log::info("ns", "{} identified as {} ({})", m.sender, a.account, nick);
    });

    c.on_command("nickserv", "LOGOUT", [](ctx &c, cmsg const &m) {
      irc::user *u = c.net().find_user(m.sender);
      if (u && u->account.empty()) {
        c.notice(m, "You are not identified.");
        return;
      }
      set_account(c, m.sender, "");
      c.notice(m, "You have been logged out.");
    });

    c.on_command("nickserv", "STATUS", [](ctx &c, cmsg const &m) {
      std::string nick = m.arg(0);
      irc::user *u = c.net().find_user(m.sender);
      if (nick.empty() && u)
        nick = u->nick;
      if (nick.empty())
        return;
      account a = load_account(c, fold(nick));
      if (!a.exists) {
        c.notice(m, "Nickname '" + nick + "' is not registered.");
        return;
      }
      bool identified = false;
      if (irc::user *t = c.net().by_nick(nick))
        identified = !t->account.empty() && t->account == a.account;
      c.notice(m, "Nickname '" + nick + "' is registered" +
                      (identified ? " and is currently identified."
                                  : "."));
      if (identified)
        c.notice(m, "Account: " +
                        (a.account.empty() ? nick : a.account));
    });

    c.on_command("nickserv", "GHOST", [](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: GHOST <nick> [password]");
        return;
      }
      std::string const &targetnick = m.arg(0);
      irc::user *tu = c.net().by_nick(targetnick);
      if (!tu) {
        c.notice(m, "No such user '" + targetnick + "'.");
        return;
      }
      account a = load_account(c, fold(targetnick));
      if (!a.exists) {
        c.notice(m, "That nickname is not registered.");
        return;
      }
      irc::user *self = c.net().find_user(m.sender);
      bool authed =
          self && !self->account.empty() && self->account == a.account;
      if (!authed) {
        if (m.argc() < 2 || !check_pass(a.password, m.arg(1))) {
          c.notice(
              m,
              "You do not hold that nickname and supplied no valid password.");
          return;
        }
      }
      irc::message mk;
      mk.prefix = m.service->uid;
      mk.command = "KILL";
      mk.params.push_back(tu->uid);
      mk.params.push_back("Ghost session reset by NickServ");
      c.deliver(mk);
      c.notice(m, "Sessions under '" + targetnick + "' have been reset.");
    });

    c.on_command("nickserv", "SET", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: SET PASSWORD <new> | SET EMAIL <new>");
        return;
      }
      irc::user *u = c.net().find_user(m.sender);
      if (!u || u->account.empty()) {
        c.notice(m, "You must be identified to change your details.");
        return;
      }
      std::string const &key = m.arg(0);
      std::string const &val = m.arg(1);
      if (sv::equals_ci(key, "PASSWORD")) {
        if (val.size() < 6) {
          c.notice(m, "Passwords must be at least 6 characters long.");
          return;
        }
        auto rows = c.database().query(
            "SELECT name FROM nickserv WHERE account=? LIMIT 1", {u->account});
        if (rows.empty())
          return;
        std::string const nm = rows[0].as_string("name");
        c.database().run("UPDATE nickserv SET password=?, salt='' WHERE name=?",
                         {hash_pass(val), nm});
        c.notice(m, "Password updated.");
      } else if (sv::equals_ci(key, "EMAIL")) {
        if (val.find('@') == std::string::npos ||
            val.find('.') == std::string::npos) {
          c.notice(m, "Invalid email address.");
          return;
        }
        auto rows = c.database().query(
            "SELECT account FROM nickserv WHERE account=? LIMIT 1",
            {u->account});
        if (rows.empty())
          return;
        c.database().run("UPDATE nickserv SET email=? WHERE account=?",
                         {val, u->account});
        c.notice(m, "Email updated.");
      } else if (sv::equals_ci(key, "UMODES")) {
        if (val.empty()) {
          c.notice(m, "Usage: SET UMODES <modes>  (e.g. +i or +i-d)");
          return;
        }
        auto rows = c.database().query(
            "SELECT name FROM nickserv WHERE account=? LIMIT 1", {u->account});
        if (rows.empty())
          return;
        std::string const nm = rows[0].as_string("name");
        std::string const allowed = k_allowed_umodes;
        std::string delta;
        char sign = '+';
        for (char ch : val) {
          if (ch == '+') {
            sign = '+';
            continue;
          }
          if (ch == '-') {
            sign = '-';
            continue;
          }
          if (ch == ' ' || ch == ',')
            continue;
          if (ch == 'r')
            continue; // the registered mode is automatic
          if (allowed.find(ch) == std::string::npos) {
            c.notice(m, std::string("Usermode '") + ch +
                            "' cannot be set. Allowed: i x d c R D B T S w N "
                            "I g W L z h");
            return;
          }
          if (delta.empty() || delta.back() == '+' || delta.back() == '-')
            delta += sign;
          else if (delta.back() != sign)
            delta += sign;
          delta += ch;
        }
        if (delta.empty()) {
          c.notice(m, "No usable modes in that setting.");
          return;
        }
        c.database().run("UPDATE nickserv SET umodes=? WHERE name=?",
                         {delta, nm});
        apply_umode(c, m.sender, delta);
        c.notice(m, "Persisted umodes; applied now and on future logins: " +
                        delta);
      } else
        c.notice(m, "Unknown SET option.");
    });

    c.on_command("nickserv", "INFO", [](ctx &c, cmsg const &m) {
      std::string nick = m.arg(0);
      if (nick.empty()) {
        irc::user *u = c.net().find_user(m.sender);
        nick = u ? u->nick : std::string();
      }
      if (nick.empty())
        return;
      account a = load_account(c, fold(nick));
      if (!a.exists) {
        c.notice(m, "Nickname '" + nick + "' is not registered.");
        return;
      }
      c.notice(m, "Nickname:   " + nick);
      c.notice(m, "Account:    " + (a.account.empty() ? nick : a.account));
      c.notice(m, "Registered: " + ts_display(a.registered));
      c.notice(m, "Last seen:  " + ts_display(a.lastseen));
      c.notice(m, "Email:      " +
                      (a.email.empty() ? std::string("(not set)") : a.email));
    });

    c.on_command("nickserv", "DROP", [](ctx &c, cmsg const &m) {
      std::string nick = m.arg(0);
      irc::user *u = c.net().find_user(m.sender);
      if (nick.empty())
        nick = u ? u->nick : std::string();
      if (nick.empty())
        return;
      std::string const folded = fold(nick);
      account a = load_account(c, folded);
      if (!a.exists) {
        c.notice(m, "Nickname '" + nick + "' is not registered.");
        return;
      }
      bool allowed = u && u->account == a.account;
      if (!allowed && is_oper(c, m.sender))
        allowed = true;
      if (!allowed) {
        c.notice(m, "You do not have permission to drop that nickname.");
        return;
      }
      c.database().run("DELETE FROM nickserv WHERE name=?", {folded});
      // clean up channel founders just in case
      c.database().run(
          "UPDATE chanserv SET founder='' , password='' WHERE founder=?",
          {folded});
      if (u && u->account == a.account)
        set_account(c, m.sender, "");
      c.notice(m, "Nickname '" + nick + "' dropped.");
    });

    // Auto-identify when a known client certificate is seen on a connection.
    c.on_user_cert = [&c](irc::user &u) {
      if (u.certfp.empty() || !u.account.empty())
        return;
      std::string acct;
      if (!cert_account(c, u.certfp, acct))
        return;
      set_account(c, u.uid, acct);
      apply_saved_umodes(c, u.uid, fold(acct));
      log::info("ns", "{} ({}@{}) auto-identified via certificate", u.nick,
                u.displayuser, u.displayhost);
    };
  }

} // namespace svc::core