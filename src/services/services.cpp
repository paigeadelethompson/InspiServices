// InspiServices - central daemon context.
#include "services/services/core.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "services/net/socket.h"
#include "services/services/syntax.h"
#include "services/util/env.h"
#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::core {

  namespace {
    std::string ascii_upper(std::string_view s) {
      std::string out(s);
      for (char &c : out)
        if (c >= 'a' && c <= 'z')
          c = static_cast<char>(c - ('a' - 'A'));
      return out;
    }

    // Environment override for a per-service field. Name is
    // SERVICES_<NAME>_<FIELD> or <NAME>_<FIELD>, e.g. NICKSERV_NICK
    // / SERVICES_NICKSERV_HOST.
    std::string env_service_field(std::string_view service,
                                  std::string_view field, std::string def) {
      std::string const base = ascii_upper(service);
      std::string const fup = ascii_upper(field);
      if (auto v = svc::env::get("SERVICES_" + base + "_" + fup);
          v && !v->empty())
        return *v;
      if (auto v = svc::env::get(base + "_" + fup); v && !v->empty())
        return *v;
      return def;
    }

    // Applies environment configuration for a service pseudo-user so each
    // bot's nickname and client host are configurable without recompiling.
    void apply_env_service(service_info &sv) {
      std::string const base = sv.name; // original service identity
      sv.name = env_service_field(base, "NICK", sv.name);
      sv.host = env_service_field(base, "HOST", sv.host);
      sv.gecos = env_service_field(base, "GECOS", sv.gecos);
      sv.ident = env_service_field(base, "IDENT", sv.ident);
    }
  } // namespace

  ctx::ctx(net::Reactor &reactor, db &database, config &cfg, irc::hub &hub)
      : reactor_(reactor), db_(database), cfg_(cfg), hub_(hub) {
    hub_.on_message = [this](irc::link &, irc::message const &m) {
      on_line(m);
    };
  }

  ctx::~ctx() = default;

  // ---------------------------------------------------------------------------
  // service management
  // ---------------------------------------------------------------------------
  service_info &ctx::add_service(std::string name, std::string gecos,
                                 bool oper) {
    std::string const sid = the_hub().cfg().server_sid;
    if (sid.size() != 3)
      throw svc::config_error("server SID must be a 3-character SID");

    // InspIRCd requires UUIDs to be exactly 3 (SID) + 6 characters and to
    // begin with our SID, else the link throws "Bogus UUID".
    std::string uid;
    bool unique = false;
    while (!unique) {
      uid = sid + std::to_string(100000 + uid_counter_++);
      unique = (uid.size() == 9) && (net().find_user(uid) == nullptr);
    }

    auto si = std::make_shared<service_info>();
    si->name = std::move(name);
    si->ident = si->name;
    for (char &c : si->ident)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    si->uid = uid;
    si->gecos = std::move(gecos);
    si->oper = oper;
    services_.push_back(si);
    return *si;
  }

  void ctx::on_command(std::string_view service, std::string_view cmd,
                       command_fn fn) {
    std::string sv = sv::irc_lower(service);
    std::string c = ascii_upper(cmd);
    commands_[sv][c] = std::move(fn);
  }

  void ctx::add_help(std::string_view service, std::string_view subject,
                     std::string text) {
    help_[sv::irc_lower(service)][ascii_upper(subject)] = std::move(text);
  }

  void ctx::help_command(cmsg const &m) {
    auto svc = help_.find(sv::irc_lower(m.service->name));
    if (svc == help_.end())
      return;
    auto const &h = svc->second;
    auto emit = [&](std::string_view line) { send_notice(*m.service, m.reply, line); };

    std::string const sub = m.arg(0);
    if (sub.empty()) {
      emit(std::string(m.service->name) + " commands (use HELP <command> for "
           "syntax):");
      for (auto const &[subject, text] : h) {
        std::string const usage = text.substr(0, text.find('\n'));
        emit(std::string("  ") + subject + " -- " + usage);
      }
      return;
    }
    auto const it = h.find(ascii_upper(sub));
    if (it == h.end()) {
      // Cross-service help: "HELP CHANSERV" on OperServ shows ChanServ's
      // command list and points the user at the right service.
      for (auto const &svc : services_) {
        if (!sv::irc_equals(sub, svc->name))
          continue;
        auto const other = help_.find(sv::irc_lower(svc->name));
        emit(svc->name + " commands (use /msg " + svc->name +
             " HELP <command> for syntax):");
        if (other != help_.end()) {
          for (auto const &[subject, text] : other->second) {
            std::string const usage =
                text.substr(0, text.find('\n'));
            emit(std::string("  ") + subject + " -- " + usage);
          }
        }
        return;
      }
      emit("No help is available for '" + sub + "'.");
      return;
    }
    std::string_view rest = it->second;
    while (!rest.empty()) {
      std::size_t nl = rest.find('\n');
      if (nl == std::string_view::npos) {
        emit(rest);
        break;
      }
      emit(rest.substr(0, nl));
      rest.remove_prefix(nl + 1);
    }
  }

  void ctx::add_channel_message(std::function<void(irc::message const &)> fn) {
    channel_messages_.push_back(std::move(fn));
  }
  void ctx::add_channel_state(std::function<void(irc::channel &)> fn) {
    channel_states_.push_back(std::move(fn));
  }
  void ctx::add_fantasy(std::function<void(irc::user const &, std::string_view,
                                           std::string_view)> fn) {
    fantasies_.push_back(std::move(fn));
  }

  void ctx::note_seen(std::string_view nick, std::int64_t ts) {
    if (!nick.empty())
      seen_[sv::irc_lower(nick)] = ts;
  }
  std::int64_t ctx::seen_at(std::string_view nick) const {
    auto it = seen_.find(sv::irc_lower(nick));
    return it == seen_.end() ? 0 : it->second;
  }

  // ---------------------------------------------------------------------------
  // sending
  // ---------------------------------------------------------------------------
  void ctx::send_notice(service_info const &sv, std::string_view target,
                        std::string_view text) {
    irc::message m;
    m.prefix = sv.uid;
    m.command = "NOTICE";
    m.params.emplace_back(target);
    m.params.emplace_back(text);
    deliver(m);
  }

  void ctx::reply(cmsg const &m, std::string_view text) {
    send_notice(*m.service, m.reply, text);
  }
  void ctx::notice(cmsg const &m, std::string_view text) {
    send_notice(*m.service, m.reply, text);
  }

  void ctx::deliver(irc::message const &m) { hub_.broadcast(m); }

  // ---------------------------------------------------------------------------
  // burst / install
  // ---------------------------------------------------------------------------
  void ctx::introduce_to(irc::link &link) {
    std::string const sid = the_hub().cfg().server_sid;
    for (auto &sv : services_) {
      svc::irc::user u;
      u.uid = sv->uid;
      u.nick = sv->name;
      u.realhost = sv->host;
      u.displayhost = sv->host;
      u.realuser = sv->ident;
      u.displayuser = sv->ident;
      u.ip = "127.0.0.1";
      u.signon = u.ts = svc::irc::now();
      u.mode = sv->oper ? "+io" : "+i";
      u.realname = sv->gecos;
      net().add_user(u);

      irc::message m;
      m.prefix = sid;
      m.command = "UID";
      m.params = {sv->uid,
                  std::to_string(u.ts),
                  sv->name,
                  u.realhost,
                  u.displayhost,
                  u.realuser,
                  u.displayuser,
                  u.ip,
                  std::to_string(u.signon),
                  u.mode,
                  sv->gecos};
      link.send(m);
      log::debug("core", "  UID {} {} @ {}", sv->uid, sv->name, sv->host);
    }
    if (on_burst_extra)
      on_burst_extra(link);
  }

  std::string ctx::service_uid(std::string_view name) const {
    for (auto &s : services_)
      if (sv::irc_equals(s->name, name))
        return s->uid;
    return {};
  }

  std::string ctx::allocate_uid() {
    std::string const sid = the_hub().cfg().server_sid;
    std::string uid;
    bool unique = false;
    while (!unique) {
      uid = sid + std::to_string(100000 + uid_counter_++);
      unique = (uid.size() == 9) && (net().find_user(uid) == nullptr);
    }
    return uid;
  }

  void ctx::install() {
    log::info("core", "installing service users");
    auto &ns = add_service("NickServ", "Nickname Service");
    auto &cs = add_service("ChanServ", "Channel Service");
    auto &bs = add_service("BotServ", "Bot Service");
    auto &os = add_service("OperServ", "Oper Service", true);
    auto &brs = add_service("BridgeServ", "Bridge Service", true);

    // Per-bot nickname/host/ident/gecos come from the environment.
    apply_env_service(ns);
    apply_env_service(cs);
    apply_env_service(bs);
    apply_env_service(os);
    apply_env_service(brs);

    install_nickserv(*this);
    install_chanserv(*this);
    install_botserv(*this);
    install_operserv(*this);
    install_bridgeserv(*this);

    // Every service answers to the generic HELP command backed by add_help().
    for (auto *svc : {"nickserv", "chanserv", "botserv", "operserv",
                      "bridgeserv"}) {
      on_command(svc, "HELP", [this](ctx &, cmsg const &m) { help_command(m); });
    }
  }

  // ---------------------------------------------------------------------------
  // inbound message handling
  // ---------------------------------------------------------------------------
  void ctx::on_line(irc::message const &m) {
    if (m.empty())
      return;
    log::debug("core", "rx {} {} {}", m.command, m.prefix,
               m.params.empty() ? "" : m.params[0]);

    if (m.command == "UID")
      handle_uid(m);
    else if (m.command == "FJOIN")
      handle_fjoin(m);
    else if (m.command == "IJOIN")
      handle_ijoin(m);
    else if (m.command == "PART")
      handle_part(m);
    else if (m.command == "KICK")
      handle_kick(m);
    else if (m.command == "NICK")
      handle_nick(m);
    else if (m.command == "QUIT")
      handle_quit(m);
    else if (m.command == "SQUIT")
      handle_squit(m);
    else if (m.command == "FTOPIC")
      handle_topic(m);
    else if (m.command == "FMODE")
      handle_fmode(m);
    else if (m.command == "AWAY")
      handle_away(m);
    else if (m.command == "OPERTYPE")
      handle_opertype(m);
    else if (m.command == "MODE")
      handle_mode(m);
    else if (m.command == "PRIVMSG" || m.command == "NOTICE")
      handle_privmsg(m);
    else if (m.command == "METADATA")
      handle_account(m);
  }

  void ctx::handle_uid(irc::message const &m) {
    // UID <uid> <ts> <nick> <realhost> <dhost> <realuser> <duser> <ip> <signon>
    // <modes> :<real>
    irc::user u;
    u.uid = m.param_or(0);
    u.ts = sv::parse_or(m.param_or(1), std::int64_t(0));
    u.nick = m.param_or(2);
    u.realhost = m.param_or(3);
    u.displayhost = m.param_or(4);
    u.realuser = m.param_or(5);
    u.displayuser = m.param_or(6);
    u.ip = m.param_or(7);
    u.signon = sv::parse_or(m.param_or(8), std::int64_t(0));
    u.mode = m.param_or(9);
    u.realname = m.params.size() > 10 ? m.params[10] : std::string();
    net().add_user(u);
  }

  void ctx::handle_fjoin(irc::message const &m) {
    // FJOIN <chan> <ts> <modes> <params...> :<members>
    // where each member is "[prefixmodes,]uuid[:membid]".
    if (m.params.size() < 3)
      return;
    std::string const &chan = m.params[0];
    irc::channel &c = net().get_channel(chan);
    c.modelock = sv::parse_or(m.params[1], std::int64_t(0));

    auto members = irc::parse_member_list(m.params.back());
    for (auto const &one : members) {
      // Strip any ":membid" membership-id suffix.
      std::string_view token(one);
      if (std::size_t const colon = token.rfind(':');
          colon != std::string_view::npos)
        token.remove_suffix(token.size() - colon);

      std::string flags;
      std::string_view uid;
      if (std::size_t const comma = token.find(',');
          comma != std::string_view::npos) {
        flags = std::string(token.substr(0, comma));
        uid = token.substr(comma + 1);
      } else
        uid = token;
      if (uid.empty())
        continue;
      c.members[std::string(uid)] = flags;
    }
    for (auto const &fn : channel_states_)
      fn(c);
  }

  void ctx::handle_ijoin(irc::message const &m) {
    // :<uid> IJOIN <chan> <membid> [<chan-age> <prefixmodes>]
    if (m.prefix.empty() || m.params.size() < 2)
      return;
    irc::channel &c = net().get_channel(m.params[0]);
    if (m.params.size() >= 3)
      c.modelock = sv::parse_or(m.params[2], c.modelock);
    // prefix modes appear as the 4th parameter when present.
    std::string flags = m.params.size() >= 4 ? m.params[3] : std::string();
    c.members[m.prefix] = flags;
    if (on_user_join) {
      if (irc::user *u = net().find_user(m.prefix))
        on_user_join(*u, c);
    }
    for (auto const &fn : channel_states_)
      fn(c);
  }

  void ctx::handle_part(irc::message const &m) {
    // :<uid> PART <chan> [:reason]
    if (m.prefix.empty() || m.params.empty())
      return;
    if (irc::channel *c = net().find_channel(m.params[0]))
      c->members.erase(m.prefix);
  }

  void ctx::handle_kick(irc::message const &m) {
    // :<src> KICK <chan> <uuid> [<membid>] :<reason>
    if (m.params.size() < 2)
      return;
    if (irc::channel *c = net().find_channel(m.params[0]))
      c->members.erase(m.params[1]);
  }

  void ctx::handle_topic(irc::message const &m) {
    // FTOPIC <chan> <chan-age> <topicset> [<setby>] :<topic>
    // (live: no setby; burst: setby present)
    if (m.params.size() < 4)
      return;
    irc::channel &c = net().get_channel(m.params[0]);
    c.modelock = sv::parse_or(m.params[1], c.modelock);
    c.topicset = sv::parse_or(m.params[2], std::int64_t(0));
    if (m.params.size() >= 5)
      c.topicsetby = m.params[3];
    c.topic = m.params.back();
  }

  void ctx::handle_squit(irc::message const &m) {
    // SQUIT <sid> :<reason> -- remove that server and everyone behind it.
    if (m.params.empty())
      return;
    std::string const sid = m.params[0];
    std::vector<irc::user *> doomed;
    for (irc::user *u : net().all_users())
      if (u->uid.compare(0, sid.size(), sid) == 0)
        doomed.push_back(u);
    for (irc::user *u : doomed) {
      for (auto *ch : net().all_channels())
        ch->members.erase(u->uid);
      net().remove_user(u->uid);
    }
  }

  void ctx::handle_away(irc::message const &m) {
    // :<uid> AWAY [:<msg>] or AWAY
    irc::user *u = net().find_user(m.prefix);
    if (!u)
      return;
    u->awaymsg = m.params.empty() ? std::string() : m.params.back();
  }

  void ctx::handle_fmode(irc::message const &m) {
    // FMODE <chan> <chan-age> <modeletters> [<params>...]  (channel modes)
    if (m.params.size() < 3)
      return;
    irc::channel &c = net().get_channel(m.params[0]);
    c.modelock = sv::parse_or(m.params[1], c.modelock);
  }

  void ctx::handle_opertype(irc::message const &m) {
    // OPERTYPE <classname> with prefix = the operator's uid. This is how the
    // ircd announces which oper class someone was promoted into; record it so
    // the services treat them as an IRC operator regardless of user modes.
    irc::user *u = net().find_user(m.prefix);
    if (!u)
      return;
    u->opertype = m.params.empty() ? std::string() : m.params[0];
  }

  void ctx::handle_mode(irc::message const &m) {
    // Usermode changes on the spanning tree arrive as:
    //   :<who> MODE <targetuuid> [+|-]<modes> [mode params...]
    // (channel modes travel as FMODE and are handled separately). Keep the
    // local model's mode string in step with the ircd.
    if (m.params.empty())
      return;
    irc::user *u = net().find_user(m.params[0]);
    if (!u)
      return;
    std::string add, sub;
    bool plus = false;
    bool saw_sign = false;
    for (std::size_t i = 1; i < m.params.size(); ++i) {
      std::string_view tok = m.params[i];
      if (tok.empty() || (tok[0] != '+' && tok[0] != '-'))
        continue; // a mode parameter (e.g. snomask), irrelevant to our model
      saw_sign = true;
      plus = tok[0] == '+';
      for (std::size_t j = 1; j < tok.size(); ++j)
        (plus ? add : sub) += tok[j];
    }
    if (!saw_sign)
      return;
    std::string nm;
    for (char ch : u->mode)
      if (ch != ' ' && sub.find(ch) == std::string::npos &&
          add.find(ch) == std::string::npos)
        nm += ch;
    for (char ch : add)
      if (nm.find(ch) == std::string::npos)
        nm += ch;
    u->mode = nm;
    log::debug("core", "umodes for {} now {}", m.params[0], nm);
  }

  void ctx::handle_nick(irc::message const &m) {
    // NICK <newnick> :<ts> with prefix = uid of the renaming user.
    irc::user *u = net().find_user(m.prefix);
    if (!u || m.params.empty())
      return;
    u->ts = sv::parse_or(m.param_or(1), std::int64_t(0));
    net().rename_user(m.prefix, m.params[0]);
  }

  void ctx::handle_quit(irc::message const &m) {
    for (auto *ch : net().all_channels())
      ch->members.erase(m.prefix);
    net().remove_user(m.prefix);
  }

  void ctx::handle_privmsg(irc::message const &m) {
    // PRIVMSG <target> :<text>
    if (m.params.size() < 2)
      return;
    std::string const &target = m.params[0];
    std::string const &text = m.params[1];

    irc::user *su = net().find_user(m.prefix);
    if (!su)
      return;

    // A message to a channel is relayed to any pluggable handler (bridge,
    // seen-tracker) and updates the last-seen cache.
    if (target.size() > 0 && (target[0] == '#' || target[0] == '&')) {
      note_seen(su->nick, svc::irc::now());
      for (auto const &fn : channel_messages_)
        fn(m);
    }

    // Fantasy (in-channel) commands: registered channels can be managed with
    // "!say", "!op", etc. through BotServ. The receiver (BotServ) decides
    // whether the message uses that channel's configured fantasy operator, so
    // every channel-targeted message is forwarded here.
    if (target.size() > 1 && target[0] == '#' && !text.empty()) {
      for (auto const &fn : fantasies_)
        fn(*su, target, text);
    }

    // Is the target one of our services (by uid or nick)?
    service_info *svc = nullptr;
    for (auto &s : services_) {
      if (s->uid == target || sv::irc_equals(s->name, target)) {
        svc = s.get();
        break;
      }
    }
    if (!svc)
      return;

    cmsg cm;
    cm.service = svc;
    cm.sender = su->uid;
    cm.nick = su->nick;
    cm.sender_account = su->account;
    cm.reply = su->uid;
    cm.text = text;

    std::vector<std::string> words = split_words(text);
    if (words.empty())
      return;
    cm.command = ascii_upper(words[0]);
    cm.args.assign(words.begin() + 1, words.end());

    std::string sv = sv::irc_lower(svc->name);
    auto found = commands_.find(sv);
    if (found == commands_.end())
      return;
    auto it = found->second.find(cm.command);
    if (it == found->second.end()) {
      send_notice(*svc, cm.reply,
                  "Unknown command. Type HELP for a list of commands.");
      return;
    }
    log::debug("core", "{} > {}: {} {}", su->nick, svc->name, cm.command,
               cm.join(0));
    it->second(*this, cm);
  }

  void ctx::handle_account(irc::message const &m) {
    // METADATA <target> <key> :<value>
    if (m.params.size() < 3)
      return;
    std::string const &target = m.params[0];
    std::string const &key = m.params[1];
    irc::user *u = net().find_user(target);
    // InspIRCd uses "accountname" for the authenticated account; a blank
    // value means logout.
    if (key == "accountname") {
      if (u) {
        u->account = m.params[2] == "0" ? std::string() : m.params[2];
      }
    } else if (key == "certfp") {
      // Client TLS certificate fingerprint (from SASL/certfp or the ircd).
      // Bindings are checked by NickServ CERT, which auto-identifies the user.
      if (u) {
        u->certfp = m.params[2] == "0" ? std::string() : m.params[2];
        if (on_user_cert)
          on_user_cert(*u);
      }
    }
  }

} // namespace svc::core