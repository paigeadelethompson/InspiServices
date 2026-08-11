// AnswerServices - bridge (remote community / platform) link manager.
#include "services/bridge/bridge.h"

#include <algorithm>

#include "services/util/env.h"
#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::bridge {

  namespace {
    bridge_kind manager_kind(std::string_view s) {
      if (sv::equals_ci(s, "discord"))
        return bridge_kind::discord;
      if (sv::equals_ci(s, "signal"))
        return bridge_kind::signal;
      return bridge_kind::link;
    }

    std::string kind_to_string(bridge_kind k) {
      switch (k) {
      case bridge_kind::discord:
        return "discord";
      case bridge_kind::signal:
        return "signal";
      default:
        return "link";
      }
    }
  } // namespace

  manager::manager(db &db, config &cfg, irc::hub &hub)
      : db_(db), cfg_(cfg), hub_(hub) {}

  void manager::load() {
    for (auto &row :
         db_.query("SELECT name, kind, server_host, port, token_env, account, "
                   "send_tls, enabled FROM bridges")) {
      bridge_config b = bind_row(row);
      bridges_.push_back(b);
      if (b.enabled) {
        switch (b.kind) {
        case bridge_kind::link:
          ensure_link(b);
          break;
        case bridge_kind::discord:
          ensure_discord(b);
          break;
        case bridge_kind::signal:
          ensure_signal(b);
          break;
        }
      }
    }
  }

  void manager::shutdown() {
    // Links are owned by the hub; platform bridges self-close on destruction.
    signals_.clear();
    discords_.clear();
  }

  bridge_config manager::bind_row(db::row const &r) const {
    bridge_config b;
    b.name = r.as_string("name");
    b.kind = manager_kind(r.as_string("kind"));
    b.server_host = r.as_string("server_host");
    b.port = r.as_string("port");
    b.account = r.as_string("account");
    if (b.port.empty())
      b.port = b.kind == bridge_kind::signal ? "7583" : "6697";
    b.token_env = r.as_string("token_env");
    b.send_tls = r.as_bool("send_tls", true);
    b.enabled = r.as_bool("enabled", true);
    return b;
  }

  void manager::ensure_link(bridge_config const &b) {
    std::string const secret = resolve_secret(b.token_env);
    if (secret.empty()) {
      log::warn("bridge", "bridge '{}': no secret in ${} (or SERVICES_{})",
                b.name, b.token_env, b.token_env);
      return;
    }
    // One InspIRCd link per link-bridge (keyed by name).
    if (links_.contains(b.name))
      return;

    irc::link_config lc;
    lc.host = b.server_host;
    lc.port = b.port;
    lc.send_tls = b.send_tls;
    lc.send_pass = secret;
    lc.recv_pass = secret;

    irc::link &l = hub_.add_uplink(lc);
    links_[b.name] = &l;
    log::info("bridge", "link to bridge '{}' configured ({}:{})", b.name,
              b.server_host, b.port);
  }

  void manager::ensure_discord(bridge_config const &b) {
    if (discords_.contains(b.name))
      return;
    // The bot token is stored verbatim in `bridges.token_env` (set via the
    // BridgeServ DISCORD command); it is never read from the environment.
    std::string const token = b.token_env;
    if (token.empty()) {
      log::warn("bridge", "discord bridge '{}': no bot token configured", b.name);
      return;
    }
    std::string host =
        b.server_host.empty() ? "gateway.discord.gg" : b.server_host;

    std::string const name = b.name;
    auto [it, inserted] =
        discords_.try_emplace(name, hub_.reactor(), std::move(host), token);
    if (!inserted)
      return;
    discord_bridge &db_br = it->second;
    db_br.on_message = [this, name = b.name](std::string_view channel,
                                             std::string_view who,
                                             std::string_view text) {
      if (on_platform_message)
        on_platform_message(name, channel, who, text);
    };
    db_br.on_state = [name = b.name](bool up) {
      log::info("bridge", "discord bridge '{}' {}", name,
                up ? "connected" : "disconnected");
    };
    db_br.start();
  }

  void manager::ensure_signal(bridge_config const &b) {
    if (signals_.contains(b.name))
      return;
    std::string host = b.server_host.empty() ? "127.0.0.1" : b.server_host;
    std::string port = b.port.empty() ? "7583" : b.port;
    std::string const name = b.name;

    auto [it, inserted] = signals_.try_emplace(
        name, hub_.reactor(), std::move(host), std::move(port), b.account);
    if (!inserted)
      return;
    signal_bridge &sb = it->second;
    sb.on_message = [this, name = b.name](std::string_view target,
                                          std::string_view who,
                                          std::string_view text) {
      if (on_platform_message)
        on_platform_message(name, target, who, text);
    };
    sb.on_state = [name = b.name](bool up) {
      log::info("bridge", "signal bridge '{}' {}", name,
                up ? "connected" : "disconnected");
    };
    sb.start();
  }

  bool manager::add(bridge::bridge_config const &b) {
    // Prevent duplicates.
    for (auto const &existing : bridges_)
      if (existing.name == b.name)
        return false;
    db_.run("INSERT INTO bridges (name, kind, server_host, port, token_env, "
            "account, send_tls, enabled) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            {b.name, kind_to_string(b.kind), b.server_host, b.port, b.token_env,
             b.account, b.send_tls ? 1 : 0, b.enabled ? 1 : 0});
    bridges_.push_back(b);
    if (b.enabled) {
      switch (b.kind) {
      case bridge_kind::link:
        ensure_link(b);
        break;
      case bridge_kind::discord:
        ensure_discord(b);
        break;
      case bridge_kind::signal:
        ensure_signal(b);
        break;
      }
    }
    return true;
  }

  bool manager::remove(std::string_view name) {
    db_.run("DELETE FROM bridges WHERE name=?", {std::string(name)});
    db_.run("DELETE FROM bridge_channels WHERE bridge=?", {std::string(name)});
    auto it =
        std::find_if(bridges_.begin(), bridges_.end(),
                     [name](bridge_config const &b) { return b.name == name; });
    if (it == bridges_.end())
      return false;
    bridges_.erase(it);
    // Close platform/link resources.
    auto lit = links_.find(std::string(name));
    if (lit != links_.end()) {
      lit->second->close("bridge removed");
      links_.erase(lit);
    }
    discords_.erase(std::string(name));
    signals_.erase(std::string(name));
    return true;
  }

  void manager::set_enabled(std::string_view name, bool on) {
    db_.run("UPDATE bridges SET enabled=? WHERE name=?",
            {on ? 1 : 0, std::string(name)});
    std::string key(name);
    auto bit =
        std::find_if(bridges_.begin(), bridges_.end(),
                     [&key](bridge_config const &b) { return b.name == key; });
    if (bit == bridges_.end())
      return;
    bridge_config const b = *bit;
    bridges_.erase(bit);
    bridges_.push_back(b);
    bridges_.back().enabled = on;

    if (on) {
      switch (b.kind) {
      case bridge_kind::link:
        ensure_link(b);
        break;
      case bridge_kind::discord:
        ensure_discord(b);
        break;
      case bridge_kind::signal:
        ensure_signal(b);
        break;
      }
    } else {
      auto lit = links_.find(key);
      if (lit != links_.end()) {
        lit->second->close("bridge disabled");
        links_.erase(lit);
      }
      discords_.erase(key);
      signals_.erase(key);
    }
  }

  std::vector<bridge_config> manager::list() const { return bridges_; }

  std::string manager::resolve_secret(std::string_view envname) const {
    std::string name(envname);
    auto v = svc::env::get(name);
    if (v)
      return *v;
    auto prefixed = svc::env::get("SERVICES_" + name);
    return prefixed ? *prefixed : std::string();
  }

  void manager::relay_to(bridge_config const &b, std::string_view remote_id,
                         std::string_view text) {
    switch (b.kind) {
    case bridge_kind::discord: {
      auto it = discords_.find(b.name);
      if (it != discords_.end())
        it->second.send_message(remote_id, text);
      break;
    }
    case bridge_kind::signal: {
      auto it = signals_.find(b.name);
      if (it != signals_.end())
        it->second.send_message(remote_id, text);
      break;
    }
    default:
      break;
    }
  }

  // ---- Virtual server protocol API ----

  virtual_server *manager::find_vs(std::string_view bridge_name) {
    auto it = virtual_servers_.find(std::string(bridge_name));
    return it != virtual_servers_.end() ? &it->second : nullptr;
  }

  std::string manager::add_virtual_server(std::string_view bridge_name,
                                          std::string_view sid_hint) {
    // Check if already exists.
    if (find_vs(bridge_name))
      return std::string();

    // Allocate a unique SID: use sid_hint if provided, otherwise generate one.
    std::string sid;
    if (!sid_hint.empty())
      sid = std::string(sid_hint);
    else {
      // Allocate a unique 3-char SID. InspIRCd only allows a digit as the
      // first character and A-Z/digits for the next two (e.g. "8E0"), so we
      // build "9" + two letters/digits and take the first free one. (This can
      // allocate up to 36*36 SIDs.)
      constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
      for (unsigned i = 0; i < 36u * 36u; i++) {
        char buf[4];
        buf[0] = '9';
        buf[1] = alphabet[(i / 36) % 36];
        buf[2] = alphabet[i % 36];
        buf[3] = '\0';
        bool taken = false;
        for (auto const &[_, vs] : virtual_servers_)
          if (vs.sid == buf) {
            taken = true;
            break;
          }
        if (!taken && buf == hub_.cfg().server_sid)
          continue; // avoid colliding with our own SID
        if (!taken) {
          sid = buf;
          break;
        }
      }
      if (sid.empty())
        sid = "9VV";
    }

    virtual_server vs;
    vs.sid = sid;
    vs.name = std::string(bridge_name) + ".anservices.net";
    vs.desc = "Bridge virtual server";
    vs.uid_counter = 1;

    // Introduce the virtual server to the spanning tree. The remote parses
    // this as SERVER <name> <sid> [property=value ...] :<desc>; calling for
    // e.g. "SERVER name 0 sid :desc" would make the SID "0" and drop the link.
    // `burst=<ts>` marks the new node as bursting until we send ENDBURST.
    irc::message m;
    m.prefix = hub_.cfg().server_sid;
    m.command = "SERVER";
    m.params.push_back(vs.name);
    m.params.push_back(vs.sid);
    m.params.push_back("burst=" + std::to_string(svc::irc::now()));
    m.params.push_back("hidden=0");
    m.params.push_back(vs.desc);
    hub_.broadcast(m);

    // Send BURST header.
    m.prefix = vs.sid;
    m.command = "BURST";
    m.params.clear();
    m.params.push_back(std::to_string(svc::irc::now()));
    hub_.broadcast(m);

    // ENDBURST is sent after all users/channels are announced.
    // For now, send it immediately (no users yet).
    m.command = "ENDBURST";
    m.params.clear();
    hub_.broadcast(m);

    virtual_servers_.emplace(std::string(bridge_name), std::move(vs));
    log::info("bridge", "virtual server '{}' added (SID {})", bridge_name, sid);
    return sid;
  }

  void manager::remove_virtual_server(std::string_view bridge_name) {
    auto *vs = find_vs(bridge_name);
    if (!vs)
      return;

    // QUIT all users.
    for (auto const &[uid, u] : vs->users) {
      irc::message m;
      m.prefix = uid;
      m.command = "QUIT";
      m.params.push_back("Bridge disconnected");
      hub_.broadcast(m);
    }
    // SQUIT the virtual server itself: :<oursid> SQUIT <sid> :<reason>.
    irc::message sq;
    sq.prefix = hub_.cfg().server_sid;
    sq.command = "SQUIT";
    sq.params.push_back(vs->sid);
    sq.params.push_back("Bridge disconnected");
    hub_.broadcast(sq);

    virtual_servers_.erase(std::string(bridge_name));
    log::info("bridge", "virtual server '{}' removed", bridge_name);
  }

  std::string manager::add_virtual_user(std::string_view bridge_name,
                                        std::string_view nick,
                                        std::string_view ident,
                                        std::string_view gecos) {
    auto *vs = find_vs(bridge_name);
    if (!vs)
      return std::string();

    // Allocate a 9-char UID: SID + 6 hex digits.
    char uid_buf[16];
    std::snprintf(uid_buf, sizeof uid_buf, "%s%06X", vs->sid.c_str(),
                  vs->uid_counter++);
    std::string uid = uid_buf;

    virtual_user vu;
    vu.uid = uid;
    vu.nick = std::string(nick);
    vu.ident = std::string(ident);
    vu.gecos = std::string(gecos);
    vs->users[uid] = std::move(vu);

    // Emit UID message (InspIRCd 1206). The exact parameter order matters:
    //   UID <uuid> <nickchanged> <nick> <realhost> <dhost> <realuser> <duser>
    //       <ip> <signon> <+modes> :<realname>
    // The IP address is parsed by the receiving ircd, so it must be valid.
    irc::message m;
    m.prefix = vs->sid;
    m.command = "UID";
    std::string const ts = std::to_string(svc::irc::now());
    m.params.push_back(uid);                // uuid
    m.params.push_back(ts);                 // nickchanged
    m.params.push_back(std::string(nick));  // nick
    m.params.push_back("bridge.virtual");   // realhost
    m.params.push_back(std::string(uid));   // displayed host
    m.params.push_back(std::string(ident)); // realuser
    m.params.push_back(std::string(ident)); // displayed user
    m.params.push_back("127.0.0.1");        // ip
    m.params.push_back(ts);                 // signon
    m.params.push_back("+i");               // modes
    m.params.push_back(std::string(gecos)); // realname
    hub_.broadcast(m);

    return uid;
  }

  void manager::remove_virtual_user(std::string_view bridge_name,
                                    std::string_view uid_str) {
    auto *vs = find_vs(bridge_name);
    if (!vs)
      return;

    auto it = vs->users.find(std::string(uid_str));
    if (it == vs->users.end())
      return;

    irc::message m;
    m.prefix = std::string(uid_str);
    m.command = "QUIT";
    m.params.push_back("Leaving");
    hub_.broadcast(m);

    vs->users.erase(it);
  }

  void manager::fjoin_user(std::string_view bridge_name,
                           std::string_view uid_str, std::string_view channel) {
    auto *vs = find_vs(bridge_name);
    if (!vs)
      return;

    // FJOIN <channel> <ts> <modes> :<list of uids>
    irc::message m;
    m.prefix = vs->sid;
    m.command = "FJOIN";
    m.params.push_back(std::string(channel));
    m.params.push_back(std::to_string(svc::irc::now()));
    m.params.push_back("+");                  // modes (none)
    m.params.push_back(std::string(uid_str)); // list of UIDs
    hub_.broadcast(m);
  }

  void manager::send_virtual_privmsg(std::string_view bridge_name,
                                     std::string_view uid_str,
                                     std::string_view target,
                                     std::string_view text) {
    auto *vs = find_vs(bridge_name);
    if (!vs)
      return;

    // Check the user exists.
    if (!vs->users.count(std::string(uid_str)))
      return;

    irc::message m;
    m.prefix = std::string(uid_str);
    m.command = "PRIVMSG";
    m.params.push_back(std::string(target));
    m.params.push_back(std::string(text));
    hub_.broadcast(m);
  }

} // namespace svc::bridge