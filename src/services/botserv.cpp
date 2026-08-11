// AnswerServices - BotServ: channel bots with in-channel (fantasy) commands.
//
// A bot is a pseudo-user owned by this daemon. Once assigned to a registered
// channel (ASSIGN <#chan> <bot>) it joins that channel and answers fantasy
// commands typed with the channel's fantasy operator (default "!") by any
// member that has enough ChanServ access:
//   !help, !seen, !op/!deop, !voice/!devoice, !kick, !kickban, !ban, !unban,
//   !say, !topic, !identify <password>
// Per-channel settings (BotServ SET <#chan> ...) configure the bot's nick
// (BOTNICK), the fantasy trigger (FANTASYOPER) and whether replies are NOTICEs
// or ordinary channel messages (REPLY). Bot management commands are restricted
// to IRC operators.
#include "services/irc/protocol.h"
#include "services/services/core.h"
#include "services/services/modules.h"
#include "services/services/syntax.h"
#include "services/util/crypto.h"
#include "services/util/log.h"
#include "services/util/util.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace svc::core {

  namespace {

    struct botrec {
      bool exists = false;
      std::string assigned;
      std::string nick;
      std::string realname;
      std::string reply = "NOTICE";
      std::string fop;                 // "" == '!'
      std::string chan_nick;           // per-channel nick override
    };

    struct botlive {
      std::string uid;
      std::string nick;
    };

    botrec load_bot(ctx &c, std::string_view name) {
      botrec r;
      auto rows = c.database().query(
          "SELECT assigned, nick, realname, reply, fop, chan_nick FROM "
          "botserv WHERE name=? LIMIT 1",
          {std::string(name)});
      if (rows.empty())
        return r;
      r.exists = true;
      r.assigned = rows[0].as_string("assigned");
      r.nick = rows[0].as_string("nick");
      r.realname = rows[0].as_string("realname");
      r.reply = rows[0].as_string("reply");
      r.fop = rows[0].as_string("fop");
      r.chan_nick = rows[0].as_string("chan_nick");
      return r;
    }

    service_info const *find_service(ctx &c, std::string_view name) {
      for (auto const &s : c.services())
        if (sv::irc_equals(s->name, name))
          return s.get();
      return nullptr;
    }

    // Client host/identity for a bot: matches the BotServ service user.
    std::unordered_map<std::string, std::string>
        g_bots; // botserv.name -> uid

    // Brings a bot online if it is not already; returns its uid or "".
    std::string ensure_bot(ctx &c, std::string const &name) {
      botrec r = load_bot(c, name);
      if (!r.exists)
        return {};
      auto it = g_bots.find(name);
      if (it != g_bots.end())
        return it->second;

      std::string const uid = c.allocate_uid();
      std::string const nick = r.chan_nick.empty()
                                   ? (r.nick.empty() ? std::string(name)
                                                     : r.nick)
                                   : r.chan_nick;
      service_info const *svc = find_service(c, "BotServ");

      irc::user u;
      u.uid = uid;
      u.nick = nick;
      u.realhost = svc ? svc->host : std::string("services.answer");
      u.displayhost = u.realhost;
      u.realuser = "botserv";
      u.displayuser = "botserv";
      u.ip = "127.0.0.1";
      u.signon = u.ts = irc::now();
      u.mode = "+i";
      u.realname = r.realname.empty() ? std::string("Channel bot") : r.realname;
      c.net().add_user(u);

      irc::message m;
      m.prefix = c.the_hub().cfg().server_sid;
      m.command = "UID";
      m.params = {uid,
                  std::to_string(u.ts),
                  nick,
                  u.realhost,
                  u.displayhost,
                  u.realuser,
                  u.displayuser,
                  u.ip,
                  std::to_string(u.signon),
                  u.mode,
                  u.realname};
      c.deliver(m);
      g_bots[name] = uid;
      log::info("bs", "bot {} online as {} ({})", name, nick, uid);
      return uid;
    }

    // Broadcasts IJOIN for `uid` in `chan` and records it on our model.
    void join_uid(ctx &c, std::string const &uid, irc::channel &chan) {
      chan.members[uid] = "o";
      irc::message m;
      m.prefix = uid;
      m.command = "IJOIN";
      m.params.push_back(chan.name);
      m.params.push_back(std::to_string(irc::now())); // membership id
      std::int64_t ts = chan.modelock > 0 ? chan.modelock : irc::now();
      m.params.push_back(std::to_string(ts)); // channel age
      m.params.push_back("o");
      c.deliver(m);
    }

    // Changes the displayed nick of a live bot and propagates the NICK change.
    void rename_live_bot(ctx &c, std::string const &name, std::string const &uid,
                         std::string const &nick) {
      irc::user *u = c.net().find_user(uid);
      if (!u || sv::irc_equals(u->nick, nick))
        return;
      std::string const oldnick = u->nick;
      irc::message m;
      m.prefix = uid;
      m.command = "NICK";
      m.params.push_back(nick);
      m.params.push_back(std::to_string(irc::now()));
      c.deliver(m);
      c.net().rename_user(uid, nick);
      log::info("bs", "bot {} renamed {} -> {}", name, oldnick, nick);
    }

    // Name of the first bot assigned to a channel, or "" if none is.
    std::string assigned_bot_name(ctx &c, std::string_view chan) {
      auto rows = c.database().query(
          "SELECT name FROM botserv WHERE assigned=? COLLATE NOCASE LIMIT 1",
          {std::string(sv::irc_lower(chan))});
      return rows.empty() ? std::string() : rows[0].as_string("name");
    }

    // BotServ administration commands are operator-only.
    bool require_oper(ctx &c, cmsg const &m) {
      if (is_oper(c, m.sender))
        return true;
      c.notice(m, "This command is restricted to IRC operators.");
      return false;
    }

    // "Xs ago" style relative timestamp.
    std::string ago_str(std::int64_t then) {
      std::int64_t const delta = svc::irc::now() - then;
      if (delta < 60)
        return std::to_string(delta) + "s ago";
      if (delta < 3600)
        return std::to_string(delta / 60) + "m ago";
      if (delta < 86400)
        return std::to_string(delta / 3600) + "h ago";
      return std::to_string(delta / 86400) + "d ago";
    }

    // Ensure every bot assigned to `chan` is online and joined.
    void ensure_bot_joined(ctx &c, irc::channel &chan) {
      auto rows = c.database().query(
          "SELECT name FROM botserv WHERE assigned=? COLLATE NOCASE",
          {chan.name});
      for (auto const &r : rows) {
        std::string const uid = ensure_bot(c, r.as_string("name"));
        if (uid.empty())
          continue;
        if (chan.members.count(uid))
          continue;
        join_uid(c, uid, chan);
        log::info("bs", "bot {} joined {}", r.as_string("name"), chan.name);
      }
    }

    // Takes a bot offline and removes it from every channel it is in.
    void quit_bot(ctx &c, std::string const &name) {
      auto it = g_bots.find(name);
      if (it == g_bots.end())
        return;
      std::string const uid = it->second;
      for (auto *ch : c.net().all_channels()) {
        if (ch->members.erase(uid)) {
          irc::message m;
          m.prefix = uid;
          m.command = "PART";
          m.params.push_back(ch->name);
          m.params.push_back("Bot removed");
          c.deliver(m);
        }
      }
      irc::message q;
      q.prefix = uid;
      q.command = "QUIT";
      q.params.push_back("Bot removed");
      c.deliver(q);
      c.net().remove_user(uid);
      g_bots.erase(it);
      log::info("bs", "bot {} went offline", name);
    }

    // ---- fantasy (in-channel) command dispatch ------------------------------
    void fantasy(ctx &c, std::string const &botuid, irc::user const &sr,
                 std::string_view chan, std::string const &reply_mode,
                 std::string const &cmd,
                 std::vector<std::string> const &args) {
      // Reply either via NOTICE (default) or as an ordinary channel message.
      bool const to_channel = sv::equals_ci(reply_mode, "CHANNEL");
      auto say = [&](std::string_view text) {
        irc::message m;
        m.prefix = botuid;
        m.command = to_channel ? "PRIVMSG" : "NOTICE";
        m.params.push_back(std::string(chan));
        m.params.push_back(std::string(text));
        c.deliver(m);
      };
      auto who = [&](std::size_t i) {
        return i < args.size() ? args[i] : std::string();
      };

      auto fmode = [&](std::string_view modes, std::string_view param = {}) {
        irc::message m;
        m.prefix = botuid;
        m.command = "FMODE";
        irc::channel *ch = c.net().find_channel(chan);
        std::int64_t ts = ch && ch->modelock > 0 ? ch->modelock : irc::now();
        m.params.push_back(std::string(chan));
        m.params.push_back(std::to_string(ts));
        m.params.push_back(std::string(modes));
        if (!param.empty())
          m.params.push_back(std::string(param));
        c.deliver(m);
      };

      if (cmd.empty() || sv::equals_ci(cmd, "HELP") ||
          sv::equals_ci(cmd, "?")) {
        say("Fantasy commands: !help, !seen <nick>, !op [!op nick], !deop, "
            "!voice, !devoice, !kick <nick> [reason], !kickban <nick> "
            "[reason], !ban <mask>, !unban <mask>, !say <text>, !topic "
            "<text>, !identify <password>");
        return;
      }
      if (sv::equals_ci(cmd, "SEEN") || sv::equals_ci(cmd, "WHO")) {
        std::string const target = who(0);
        irc::user *tu = target.empty() ? nullptr : c.net().by_nick(target);
        std::int64_t const t = c.seen_at(target);
        if (tu)
          say(target + " is here now.");
        else if (t > 0)
          say("I last saw " + target + " " + ago_str(t) + ".");
        else
          say("I have never seen " +
              (target.empty() ? std::string("that nick") : target) +
              " speak.");
        return;
      }
      if (sv::equals_ci(cmd, "SAY")) {
        bool const okay =
            is_oper(c, sr.uid) || can_chan(c, sr, chan, 400);
        if (!okay) {
          say("Only users with PROTECT access (or an operator) can make me "
              "speak.");
          return;
        }
        std::string text;
        for (std::size_t i = 0; i < args.size(); ++i) {
          if (i)
            text.push_back(' ');
          text += args[i];
        }
        if (text.empty()) {
          say("Usage: !say <text>");
          return;
        }
        irc::message m;
        m.prefix = botuid;
        m.command = "PRIVMSG";
        m.params.push_back(std::string(chan));
        m.params.push_back(text);
        c.deliver(m);
        return;
      }
      if (sv::equals_ci(cmd, "KICKBAN") || sv::equals_ci(cmd, "KB")) {
        std::string const target = who(0);
        if (target.empty()) {
          say("Usage: !kickban <nick> [reason]");
          return;
        }
        irc::user *tu = c.net().by_nick(target);
        if (!tu) {
          say("No such user '" + target + "'.");
          return;
        }
        if (!can_chan(c, sr, chan, 500)) {
          say("You do not have permission to kick-ban in " +
              std::string(chan) + ".");
          return;
        }
        std::string const mask =
            "*!*@" + (tu->displayhost.empty() ? std::string("localhost")
                                              : tu->displayhost);
        std::string reason = who(1);
        if (reason.empty())
          reason = "Kicked by " + sr.nick;
        fmode("+b", mask);
        irc::message m;
        m.prefix = botuid;
        m.command = "KICK";
        m.params.push_back(std::string(chan));
        m.params.push_back(tu->uid);
        m.params.push_back(reason);
        c.deliver(m);
        if (irc::channel *chk = c.net().find_channel(chan))
          chk->members.erase(tu->uid);
      } else if (sv::equals_ci(cmd, "OP") || sv::equals_ci(cmd, "VOICE") ||
                 sv::equals_ci(cmd, "DEOP") ||
                 sv::equals_ci(cmd, "DEVOICE")) {
        std::string target = who(0);
        if (target.empty())
          target = sr.nick;
        irc::user *tu = c.net().by_nick(target);
        if (!tu) {
          say("No such user '" + target + "'.");
          return;
        }
        int need = (sv::equals_ci(cmd, "OP") || sv::equals_ci(cmd, "DEOP"))
                       ? 300
                       : 200;
        if (!can_chan(c, sr, chan, need)) {
          say("You do not have access to manage modes in " +
              std::string(chan) + ".");
          return;
        }
        bool add = sv::equals_ci(cmd, "OP") || sv::equals_ci(cmd, "VOICE");
        char mode = (sv::equals_ci(cmd, "OP") || sv::equals_ci(cmd, "DEOP"))
                        ? 'o'
                        : 'v';
        fmode(std::string(1, add ? '+' : '-') + mode, tu->uid);
        return;
      }
      if (sv::equals_ci(cmd, "KICK")) {
        std::string const target = who(0);
        if (target.empty()) {
          say("Usage: !kick <nick> [reason]");
          return;
        }
        irc::user *tu = c.net().by_nick(target);
        if (!tu) {
          say("No such user '" + target + "'.");
          return;
        }
        if (!can_chan(c, sr, chan, 500)) {
          say("You do not have permission to kick in " + std::string(chan) +
              ".");
          return;
        }
        std::string reason = who(1);
        if (reason.empty())
          reason = "Kicked by " + sr.nick;
        irc::message m;
        m.prefix = botuid;
        m.command = "KICK";
        m.params.push_back(std::string(chan));
        m.params.push_back(tu->uid);
        m.params.push_back(reason);
        c.deliver(m);
      } else if (sv::equals_ci(cmd, "BAN") || sv::equals_ci(cmd, "UNBAN")) {
        std::string const mask = who(0);
        if (mask.empty()) {
          say("Usage: !" + cmd + " <mask>");
          return;
        }
        if (!can_chan(c, sr, chan, 400)) {
          say("You do not have permission to change bans in " +
              std::string(chan) + ".");
          return;
        }
        bool add = sv::equals_ci(cmd, "BAN");
        fmode(std::string(1, add ? '+' : '-') + "b", mask);
      } else if (sv::equals_ci(cmd, "TOPIC")) {
        std::string topic;
        for (std::size_t i = 0; i < args.size(); ++i) {
          if (i)
            topic.push_back(' ');
          topic += args[i];
        }
        if (topic.empty()) {
          say("Usage: !topic <new topic>");
          return;
        }
        if (!can_chan(c, sr, chan, 450)) {
          say("You do not have permission to change the topic in " +
              std::string(chan) + ".");
          return;
        }
        irc::message m;
        m.prefix = botuid;
        m.command = "TOPIC";
        m.params.push_back(std::string(chan));
        m.params.push_back(topic);
        c.deliver(m);
      } else if (sv::equals_ci(cmd, "IDENTIFY")) {
        std::string const password = who(0);
        if (password.empty()) {
          say("Usage: !identify <password>");
          return;
        }
        std::string const key = sv::irc_lower(chan);
        auto rows = c.database().query(
            "SELECT password FROM chanserv WHERE name=? LIMIT 1", {key});
        if (rows.empty() || rows[0].as_string("password").empty()) {
          say("Channel '" + std::string(chan) + "' has no password set.");
          return;
        }
        if (!svc::crypto::timing_safe_equal(rows[0].as_string("password"),
                                            password)) {
          say("Incorrect channel password.");
          return;
        }
        std::string const acct =
            sr.account.empty() ? std::string(fold(sr.nick)) : fold(sr.account);
        c.database().run("INSERT OR REPLACE INTO chanserv_access (channel, "
                         "who, level) VALUES (?, ?, 500)",
                         {key, acct});
        say("You now have access to " + std::string(chan) + ".");
      } else {
        say("Unknown fantasy command. Try !help.");
      }
    }

  } // namespace

  void install_botserv(ctx &c) {

    c.add_help("botserv", "BOT",
               "Usage: BOT ADD <name> [nick] | BOT DEL <name>\n"
               "Creates/removes a bot definition. Operators only. The bot's "
               "nick defaults to its name when not given.");
    c.add_help("botserv", "ASSIGN",
               "Usage: ASSIGN <#channel> <bot>\n"
               "Assigns a bot to a registered channel; it joins immediately "
               "(or on the next burst). Operators only.");
    c.add_help("botserv", "UNASSIGN",
               "Usage: UNASSIGN <bot>\n"
               "Removes a bot from its channel and takes it offline.");
    c.add_help("botserv", "LIST",
               "Usage: LIST\n"
               "Lists the configured bots, their nicks and assignments.");
    c.add_help("botserv", "SET",
               "Usage: SET <#channel> BOTNICK <nick>\n"
               "       SET <#channel> FANTASYOPER <char>\n"
               "       SET <#channel> REPLY NOTICE|CHANNEL\n"
               "Per-channel bot settings: the nick the bot uses there, the "
               "fantasy trigger character (default !), and whether fantasy "
               "replies are NOTICEs in the channel or ordinary channel "
               "messages. Operators or the channel founder may change them.");
    c.add_help("botserv", "FANTASY",
               "In-channel fantasy commands (using the channel's operator "
               "char, e.g. !):\n"
               "  !help            list commands\n"
               "  !seen <nick>     when someone last spoke here\n"
               "  !op/!deop [nick] manage op (300+)\n"
               "  !voice/!devoice [nick] manage voice (200+)\n"
               "  !kick <nick> [r]  kick (500+)\n"
               "  !kickban <n> [r]  ban and kick (500+)\n"
               "  !ban/!unban <m>   manage bans (400+)\n"
               "  !say <text>       channel says it (400+ or oper)\n"
               "  !topic <text>     set topic (450+)\n"
               "  !identify <pw>    gain access with the channel password");

    c.on_command("botserv", "BOT", [](ctx &c, cmsg const &m) {
      if (!require_oper(c, m))
        return;
      if (m.argc() < 2) {
        c.notice(m, "Usage: BOT <ADD|DEL> <name> [nick]");
        return;
      }
      std::string const &op = m.arg(0);
      std::string const &name = m.arg(1);
      if (sv::equals_ci(op, "ADD")) {
        std::string nick = m.arg(2);
        if (nick.empty())
          nick = name;
        c.database().run(
            "INSERT OR REPLACE INTO botserv (name, assigned, nick, realname, "
            "password) VALUES (?,?,?,'', '')",
            {name, "", nick});
        c.notice(m, "Bot '" + name + "' added (nick " + nick + ").");
      } else if (sv::equals_ci(op, "DEL")) {
        quit_bot(c, name);
        c.database().run("DELETE FROM botserv WHERE name=?", {name});
        c.notice(m, "Bot '" + name + "' removed.");
      } else
        c.notice(m, "Unknown BotServ BOT operation. Use ADD or DEL.");
    });

    c.on_command("botserv", "ASSIGN", [](ctx &c, cmsg const &m) {
      if (!require_oper(c, m))
        return;
      if (m.argc() < 2) {
        c.notice(m, "Usage: ASSIGN <#channel> <bot>");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &bot = m.arg(1);
      botrec r = load_bot(c, bot);
      if (!r.exists) {
        c.notice(m, "Bot '" + bot + "' does not exist.");
        return;
      }
      c.database().run("UPDATE botserv SET assigned=? WHERE name=?",
                       {chan, bot});
      if (irc::channel *ch = c.net().find_channel(chan)) {
        std::string const uid = ensure_bot(c, bot);
        if (!uid.empty() && !ch->members.count(uid)) {
          join_uid(c, uid, *ch);
          log::info("bs", "bot {} joined {} (assign)", bot, chan);
        }
      } else {
        // Channel not seen yet; it will join via on_channel_state.
        (void)ensure_bot(c, bot);
      }
      c.notice(m, "Bot '" + bot + "' assigned to " + chan + ".");
    });

    c.on_command("botserv", "UNASSIGN", [](ctx &c, cmsg const &m) {
      if (!require_oper(c, m))
        return;
      if (m.argc() < 1) {
        c.notice(m, "Usage: UNASSIGN <bot>");
        return;
      }
      std::string const &bot = m.arg(0);
      botrec r = load_bot(c, bot);
      if (!r.exists) {
        c.notice(m, "Bot '" + bot + "' does not exist.");
        return;
      }
      quit_bot(c, bot);
      c.database().run("UPDATE botserv SET assigned='' WHERE name=?", {bot});
      c.notice(m, "Bot '" + bot + "' unassigned.");
    });

    auto list_bots = [](ctx &c, cmsg const &m) {
      if (!require_oper(c, m))
        return;
      auto rows = c.database().query(
          "SELECT name, nick, chan_nick, assigned FROM botserv ORDER BY name");
      c.notice(m, "Bots:");
      for (auto const &r : rows) {
        c.notice(m, "  " + r.as_string("nick") + " (" + r.as_string("name") +
                        ") assigned to " + r.as_string("assigned"));
      }
      if (rows.empty())
        c.notice(m, "  (no bots configured)");
    };
    c.on_command("botserv", "LIST", list_bots);
    c.on_command("botserv", "BOTLIST", list_bots);

    // ---- per-channel settings: bot nick, fantasy operator, reply style ----
    c.on_command("botserv", "SET", [](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: SET <#channel> BOTNICK <nick> | FANTASYOPER "
                    "<char> | REPLY NOTICE|CHANNEL");
        return;
      }
      std::string const &chan = m.arg(0);
      std::string const &opt = m.arg(1);
      if (m.argc() < 3 || !sv::valid_chan(chan) ||
          (!sv::equals_ci(opt, "BOTNICK") &&
           !sv::equals_ci(opt, "FANTASYOPER") &&
           !sv::equals_ci(opt, "REPLY"))) {
        c.notice(m, "Usage: SET <#channel> BOTNICK <nick> | FANTASYOPER "
                    "<char> | REPLY NOTICE|CHANNEL");
        return;
      }
      std::string const bot = assigned_bot_name(c, chan);
      if (bot.empty()) {
        c.notice(m, "No bot is assigned to '" + chan + "'.");
        return;
      }
      irc::user *self = c.net().find_user(m.sender);
      bool const founder = self && !self->account.empty() &&
                           channel_founder(c, chan) == fold(self->account);
      if (!require_oper(c, m) && !founder) {
        c.notice(m, "You must be an operator or the channel founder.");
        return;
      }
      std::string const &val = m.arg(2);
      if (sv::equals_ci(opt, "BOTNICK")) {
        if (!sv::valid_nick(val)) {
          c.notice(m, "Invalid bot nick '" + val + "'.");
          return;
        }
        c.database().run("UPDATE botserv SET chan_nick=? WHERE name=?",
                         {val, bot});
        auto it = g_bots.find(bot);
        if (it != g_bots.end())
          rename_live_bot(c, bot, it->second, val);
        c.notice(m, "Bot '" + bot + "' will use nick " + val + " in " + chan +
                        ".");
        return;
      }
      if (sv::equals_ci(opt, "FANTASYOPER")) {
        if (val.size() > 1) {
          c.notice(m, "FANTASYOPER must be a single character (e.g. ! or ?).");
          return;
        }
        c.database().run("UPDATE botserv SET fop=? WHERE name=?",
                         {val.empty() ? std::string("!") : val, bot});
        c.notice(m, "Fantasy operator for " + chan + " is now '" +
                        (val.empty() ? std::string("!") : val) + "'.");
        return;
      }
      if (sv::equals_ci(opt, "REPLY")) {
        std::string mode = "NOTICE";
        if (sv::equals_ci(val, "CHANNEL"))
          mode = "CHANNEL";
        else if (!sv::equals_ci(val, "NOTICE")) {
          c.notice(m, "REPLY must be NOTICE or CHANNEL.");
          return;
        }
        c.database().run("UPDATE botserv SET reply=? WHERE name=?",
                         {mode, bot});
        c.notice(m, "Fantasy replies in " + chan + " will be " +
                        (mode == "CHANNEL" ? std::string("channel messages")
                                           : std::string("NOTICEs")) +
                        ".");
        return;
      }
    });

    // ---- runtime wiring: keep bots joined; answer fantasy commands ----
    c.add_channel_state([&c](irc::channel &ch) { ensure_bot_joined(c, ch); });

    c.add_fantasy([&c](irc::user const &sr, std::string_view chan,
                       std::string_view text) {
      std::string const key = sv::irc_lower(chan);
      bool const registered = !c.database()
                                   .query("SELECT name FROM chanserv WHERE "
                                          "name=? LIMIT 1",
                                          {key})
                                   .empty();
      auto botrows = c.database().query(
          "SELECT name, fop, reply FROM botserv WHERE assigned=? COLLATE "
          "NOCASE LIMIT 1",
          {key});
      bool const hasbot = !botrows.empty();
      if (!registered && !hasbot)
        return;

      std::string fop = botrows.empty() ? std::string("!") : botrows[0].as_string("fop");
      if (fop.empty())
        fop = "!";
      if (text.empty() || text[0] != fop[0])
        return;

      std::string botuid;
      std::string reply = "NOTICE";
      if (!botrows.empty()) {
        botuid = ensure_bot(c, botrows[0].as_string("name"));
        reply = botrows[0].as_string("reply");
      }
      if (botuid.empty())
        botuid = c.service_uid("BotServ");

      std::string body(text.substr(1));
      std::vector<std::string> words = sv::splitws(body);
      if (words.empty())
        return;
      std::string cmd = words[0];
      for (char &ch : cmd)
        if (ch >= 'a' && ch <= 'z')
          ch = static_cast<char>(ch - ('a' - 'A'));
      std::vector<std::string> args(words.begin() + 1, words.end());
      fantasy(c, botuid, sr, chan, reply, cmd, args);
    });

    // Include assigned bots in every (re)burst.
    c.on_burst_extra = [&c](irc::link &l) {
      auto rows = c.database().query(
          "SELECT name FROM botserv WHERE assigned<>''");
      for (auto const &r : rows) {
        std::string const uid = ensure_bot(c, r.as_string("name"));
        irc::user *u = uid.empty() ? nullptr : c.net().find_user(uid);
        if (!u)
          continue;
        irc::message m;
        m.prefix = c.the_hub().cfg().server_sid;
        m.command = "UID";
        m.params = {u->uid,
                    std::to_string(u->ts),
                    u->nick,
                    u->realhost,
                    u->displayhost,
                    u->realuser,
                    u->displayuser,
                    u->ip,
                    std::to_string(u->signon),
                    u->mode,
                    u->realname};
        l.send(m);
      }
      for (auto *ch : c.net().all_channels())
        ensure_bot_joined(c, *ch);
    };
  }

} // namespace svc::core