// AnswerServices - BridgeServ: manage bridge (remote community / platform)
// links.
//
// Bridges come in two flavours:
//   * LINK  - an outbound InspIRCd link to a remote hub / community.
//   * DISCORD - a native Discord bot relay (gateway + REST).
//   * SIGNAL  - a native signal-cli JSON-RPC relay.
// BridgeServ stores the connection details in the `bridges` table, materialises
// them, and manages the channel mapping table (`bridge_channels`).
#include "services/bridge/bridge.h"
#include "services/irc/routing.h"
#include "services/services/core.h"
#include "services/services/modules.h"
#include "services/services/syntax.h"
#include "services/util/env.h"
#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::core {

  namespace {
    // Look up a bridge by name, returns nullptr if unknown.
    svc::bridge::bridge_config const *
    find_bridge(std::vector<svc::bridge::bridge_config> const &all,
                std::string_view name) {
      for (auto const &b : all)
        if (b.name == name)
          return &b;
      return nullptr;
    }

    std::string kind_name(svc::bridge::bridge_kind k) {
      switch (k) {
      case svc::bridge::bridge_kind::discord:
        return "discord";
      case svc::bridge::bridge_kind::signal:
        return "signal";
      default:
        return "link";
      }
    }
  } // namespace

  // The relay hook is wired below in install_bridgeserv itself (via
  // inst->on_platform_message). It turns an inbound platform message into a
  // PRIVMSG sent from the BridgeServ pseudo-user.

  void install_bridgeserv(ctx &c) {
    // Bridge manager must be set by main() via ctx::set_bridge_manager() before
    // any commands are executed. The `ensure` lambda returns the shared
    // instance.
    auto ensure = [&c]() -> svc::bridge::manager * {
      auto *m = c.bridge_manager();
      if (!m) {
        log::error("bridgeserv",
                   "no bridge manager set; call ctx::set_bridge_manager()");
        return nullptr;
      }
      return m;
    };

    c.add_help("bridgeserv", "LINK",
               "Usage: LINK <name> <host> [port] <tokenvar>\n"
               "Adds an outbound InspIRCd link bridge to a remote community. "
               "The link token is read from the named environment variable.");
    c.add_help("bridgeserv", "DISCORD",
               "Usage: DISCORD <name> <token> [gateway-host]\n"
               "Adds a native Discord bot relay (token stored in the DB).");
    c.add_help("bridgeserv", "SIGNAL",
               "Usage: SIGNAL <name> <account> [host] [port]\n"
               "Adds a native signal-cli JSON-RPC relay.");
    c.add_help("bridgeserv", "LIST",
               "Usage: LIST\n"
               "Lists configured bridges and their status.");
    c.add_help("bridgeserv", "DEL",
               "Usage: DEL <name>\n"
               "Removes a bridge.");
    c.add_help("bridgeserv", "ENABLE",
               "Usage: ENABLE <name>\n"
               "Enables message relay for a bridge.");
    c.add_help("bridgeserv", "DISABLE",
               "Usage: DISABLE <name>\n"
               "Disables message relay for a bridge.");
    c.add_help("bridgeserv", "MAP",
               "Usage: MAP <bridge> <#ircchannel> <remoteid>\n"
               "Maps an IRC channel to a remote (Discord channel id / Signal "
               "number or groupId).");
    c.add_help("bridgeserv", "UNMAP",
               "Usage: UNMAP <bridge> <#ircchannel>\n"
               "Removes a channel mapping.");

    c.on_command("bridgeserv", "LIST", [ensure](ctx &c, cmsg const &m) {
      auto *mgr = ensure();
      if (!mgr)
        return;
      auto lst = mgr->list();
      c.notice(m, "Configured bridges:");
      for (auto const &b : lst) {
        std::string line = "[" + kind_name(b.kind) + "] " + b.name + " (" +
                           (b.enabled ? "enabled" : "disabled") + ")";
        if (b.kind == svc::bridge::bridge_kind::link)
          line += " -> " + b.server_host + ":" + b.port +
                  " token_env=" + b.token_env;
        else if (b.kind == svc::bridge::bridge_kind::discord)
          line += " token_env=" + b.token_env;
        else
          line += " daemon=" + b.server_host + ":" + b.port +
                  " account=" + b.account;
        c.notice(m, "  " + line);
      }
      if (lst.empty())
        c.notice(m, "  (none)");
    });

    c.on_command("bridgeserv", "LINK", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 3) {
        c.notice(m, "Usage: LINK <name> <host> [port] <tokenvar>");
        return;
      }
      svc::bridge::bridge_config b;
      b.kind = svc::bridge::bridge_kind::link;
      b.name = m.arg(0);
      b.server_host = m.arg(1);
      b.port = m.arg(2);
      if (b.port.empty())
        b.port = "6697";
      b.token_env = m.arg(3);
      if (b.token_env.empty())
        b.token_env = "BRIDGE_" + b.name;
      for (char &ch : b.token_env)
        if (ch >= 'a' && ch <= 'z')
          ch = static_cast<char>(ch - ('a' - 'A'));
      auto *mgr = ensure();
      if (!mgr)
        return;
      if (!mgr->add(b))
        c.notice(m, "Could not add bridge '" + b.name + "' (duplicate?).");
      else
        c.notice(m,
                 "Link bridge '" + b.name + "' added. Map channels with MAP.");
    });

    c.on_command("bridgeserv", "DISCORD", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: DISCORD <name> <token> [gateway-host]");
        return;
      }
      svc::bridge::bridge_config b;
      b.kind = svc::bridge::bridge_kind::discord;
      b.name = m.arg(0);
      b.token_env = m.arg(1);
      b.port = "443";
      if (m.argc() > 2)
        b.server_host = m.arg(2); // optional gateway host
      else
        b.server_host = "gateway.discord.gg";
      auto *mgr = ensure();
      if (!mgr)
        return;
      if (!mgr->add(b))
        c.notice(m, "Could not add bridge '" + b.name + "' (duplicate?).");
      else
        c.notice(m, "Discord bridge '" + b.name +
                        "' added. Map channels with MAP.");
    });

    c.on_command("bridgeserv", "SIGNAL", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: SIGNAL <name> <account> [host] [port]");
        return;
      }
      svc::bridge::bridge_config b;
      b.kind = svc::bridge::bridge_kind::signal;
      b.name = m.arg(0);
      b.account = m.arg(1);
      b.server_host = m.argc() > 2 ? m.arg(2) : "127.0.0.1";
      b.port = m.argc() > 3 ? m.arg(3) : "7583";
      b.token_env = "SIGNAL_ACCOUNT";
      auto *mgr = ensure();
      if (!mgr)
        return;
      if (!mgr->add(b))
        c.notice(m, "Could not add bridge '" + b.name + "' (duplicate?).");
      else
        c.notice(m, "Signal bridge '" + b.name +
                        "' added. Map channels with MAP.");
    });

    c.on_command("bridgeserv", "DEL", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: DEL <name>");
        return;
      }
      auto *mgr = ensure();
      if (!mgr)
        return;
      if (mgr->remove(m.arg(0)))
        c.notice(m, "Bridge '" + m.arg(0) + "' removed.");
      else
        c.notice(m, "No such bridge '" + m.arg(0) + "'.");
    });

    c.on_command("bridgeserv", "ENABLE", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: ENABLE <name>");
        return;
      }
      auto *mgr = ensure();
      if (!mgr)
        return;
      mgr->set_enabled(m.arg(0), true);
      c.notice(m, "Bridge '" + m.arg(0) + "' enabled.");
    });

    c.on_command("bridgeserv", "DISABLE", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 1) {
        c.notice(m, "Usage: DISABLE <name>");
        return;
      }
      auto *mgr = ensure();
      if (!mgr)
        return;
      mgr->set_enabled(m.arg(0), false);
      c.notice(m, "Bridge '" + m.arg(0) + "' disabled.");
    });

    c.on_command("bridgeserv", "MAP", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 3) {
        c.notice(m, "Usage: MAP <bridge> <#ircchannel> <remoteid>");
        return;
      }
      auto *mgr = ensure();
      if (!mgr)
        return;
      auto lst = mgr->list();
      if (!find_bridge(lst, m.arg(0))) {
        c.notice(m, "No such bridge '" + m.arg(0) + "'.");
        return;
      }
      c.database().run(
          "INSERT OR REPLACE INTO bridge_channels (bridge, channel, remote) "
          "VALUES (?, ?, ?)",
          {m.arg(0), m.arg(1), m.arg(2)});
      c.notice(m, "Channel " + m.arg(1) + " mapped to remote " + m.arg(2) +
                      " on '" + m.arg(0) + "'.");
    });

    c.on_command("bridgeserv", "UNMAP", [ensure](ctx &c, cmsg const &m) {
      if (m.argc() < 2) {
        c.notice(m, "Usage: UNMAP <bridge> <#ircchannel>");
        return;
      }
      auto *mgr = ensure();
      if (!mgr)
        return;
      c.database().run(
          "DELETE FROM bridge_channels WHERE bridge=? AND channel=?",
          {m.arg(0), m.arg(1)});
      c.notice(m, "Channel " + m.arg(1) + " unlinked from '" + m.arg(0) + "'.");
    });

    // Wire the outbound relay: IRC PRIVMSG on bridged channels -> platform.
    c.add_channel_message([&c, ensure](irc::message const &m) {
      if (m.params.size() < 2 || m.params[0].empty())
        return;
      std::string const &target = m.params[0];
      std::string const &text = m.params[1];
      // Look up which bridge owns this channel mapping.
      svc::db &db = c.database();
      auto rows =
          db.query("SELECT bridge, remote FROM bridge_channels WHERE channel=?",
                   {target});
      auto *mgr = ensure();
      if (!mgr)
        return;
      for (auto &row : rows) {
        std::string bridge_name = row.as_string("bridge");
        std::string remote_id = row.as_string("remote");
        // Find the bridge config and relay.
        auto lst = mgr->list();
        for (auto const &b : lst) {
          if (b.name == bridge_name && b.enabled) {
            mgr->relay_to(b, remote_id, text);
            break;
          }
        }
      }
    });

    auto *mgr = ensure();
    if (!mgr)
      return;

    mgr->on_platform_message = [&c, ensure](std::string_view bridge_name,
                                            std::string_view remote,
                                            std::string_view sender,
                                            std::string_view text) {
      c.the_reactor().call_later([&c, ensure, name = std::string(bridge_name),
                                  rm = std::string(remote),
                                  who = std::string(sender),
                                  body = std::string(text)]() {
        // Look up the IRC channel for this remote id.
        svc::db &db = c.database();
        auto rows = db.query(
            "SELECT channel FROM bridge_channels WHERE bridge=? AND remote=?",
            {name, rm});
        if (rows.empty())
          return;
        std::string chan = rows[0].as_string("channel");

        auto *mgr = ensure();
        if (!mgr)
          return;

        // Ensure a virtual server exists for this bridge.
        mgr->add_virtual_server(name, {});

        // Cache virtual users per bridge+sender so we reuse UIDs.
        static std::map<std::string, std::map<std::string, std::string>>
            vu_cache;
        auto &sender_map = vu_cache[name];
        auto uit = sender_map.find(who);
        std::string uid;
        if (uit == sender_map.end()) {
          uid = mgr->add_virtual_user(name, who, "~" + who, who);
          sender_map[who] = uid;
        } else
          uid = uit->second;

        // Ensure the virtual user is FJOINed to the channel.
        mgr->fjoin_user(name, uid, chan);

        // Send the PRIVMSG.
        mgr->send_virtual_privmsg(name, uid, chan, who + ": " + body);
      });
    };
  }

} // namespace svc::core