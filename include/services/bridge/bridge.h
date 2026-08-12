// InspiServices - bridge (remote community) management.
//
// A bridge connects a platform (Discord guild, Signal account/group) to the
// IRC network via a virtual server node in the spanning tree. Each platform
// becomes its own virtual server with a unique SID; users appear as virtual
// UIDs under that server. Channel memberships are established via FJOIN burst
// (netsplit semantics), not individual JOIN commands.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "services/bridge/discord.h"
#include "services/bridge/signal.h"
#include "services/config.h"
#include "services/db.h"
#include "services/irc/routing.h"
#include "services/net/eventloop.h"

namespace svc::bridge {

  // What transport a bridge row represents.
  enum class bridge_kind {
    link,    // classic InspIRCd outbound link to a remote hub
    discord, // native Discord bot relay
    signal,  // native signal-cli JSON-RPC relay
  };

  struct bridge_config {
    std::string name; // unique id, e.g. "discord-main"
    bridge_kind kind = bridge_kind::link;
    std::string
        server_host;  // where to connect (host or tcp:host) / gateway host
    std::string port; // default "6697" (link) or "7583" (signal)
    std::string
        token_env; // env var holding the secret (link recv/send, bot token)
    std::string account; // extra config: signal account number / discord guild
    bool send_tls = true;
    bool enabled = true;
  };

  // A virtual user hosted on a virtual server.
  struct virtual_user {
    std::string uid;   // 9-char InspIRCd UID (sid + 6 hex)
    std::string nick;  // display name
    std::string ident; // username part
    std::string gecos; // real name / description
  };

  // A virtual server node in the spanning tree.
  struct virtual_server {
    std::string sid;  // 3-char SID, e.g. "8G01"
    std::string name; // e.g. "discord-main.inspiservices.net"
    std::string desc; // e.g. "Discord guild bridge"
    std::map<std::string, virtual_user> users; // uid -> user
    unsigned uid_counter = 1;                  // for allocating UIDs
  };

  class manager {
  public:
    manager(db &db, config &cfg, irc::hub &hub);

    // Loads bridge rows from the `bridges` table and creates (or reuses) a
    // hub link for each enabled bridge.
    void load();
    void shutdown();

    // BridgeServ operations.
    bool add(bridge::bridge_config const &b);
    bool remove(std::string_view name);
    void set_enabled(std::string_view name, bool on);
    std::vector<bridge_config> list() const;

    // ---- Virtual server protocol API ----
    // Each platform bridge uses these methods to emit IRC protocol events
    // through the hub, rather than forwarding plain PRIVMSG.

    // Creates a virtual server node and introduces it to the spanning tree
    // (SERVER + BURST + ENDBURST). Returns the allocated SID.
    std::string add_virtual_server(std::string_view bridge_name,
                                   std::string_view sid_hint);

    // Removes a virtual server node (sends QUIT for all users, then
    // unregisters the server).
    void remove_virtual_server(std::string_view bridge_name);

    // Adds a virtual user under a bridge's virtual server and emits a UID
    // message during burst. Returns the allocated uid.
    std::string add_virtual_user(std::string_view bridge_name,
                                 std::string_view nick, std::string_view ident,
                                 std::string_view gecos);

    // Removes a virtual user (sends QUIT).
    void remove_virtual_user(std::string_view bridge_name,
                             std::string_view uid);

    // Adds a virtual user to a channel via FJOIN during burst.
    void fjoin_user(std::string_view bridge_name, std::string_view uid,
                    std::string_view channel);

    // Sends a PRIVMSG from a virtual user to a target (channel or nick).
    void send_virtual_privmsg(std::string_view bridge_name,
                              std::string_view uid, std::string_view target,
                              std::string_view text);

    // Protocol bridge access (for the core's message relay).
    // Hands an inbound platform message to `on_platform_message`.
    std::function<void(std::string_view bridge_name, std::string_view remote_id,
                       std::string_view sender, std::string_view text)>
        on_platform_message;

    // Relays a channel message from IRC into a specific platform.
    void relay_to(bridge_config const &b, std::string_view remote_id,
                  std::string_view text);

  private:
    bridge_config bind_row(db::row const &r) const;
    void ensure_link(bridge_config const &b);
    void ensure_discord(bridge_config const &b);
    void ensure_signal(bridge_config const &b);
    std::string resolve_secret(std::string_view envname) const;

    // Returns the virtual_server for a bridge name, or nullptr.
    virtual_server *find_vs(std::string_view bridge_name);

    db &db_;
    config &cfg_;
    irc::hub &hub_;
    std::vector<bridge_config> bridges_;
    // name -> outbound link (owned by the hub).
    std::map<std::string, irc::link *, std::less<>> links_;
    // name -> native platform bridge.
    std::map<std::string, discord_bridge, std::less<>> discords_;
    std::map<std::string, signal_bridge, std::less<>> signals_;
    // name -> virtual server (for platform bridges that use spanning tree).
    std::map<std::string, virtual_server, std::less<>> virtual_servers_;
  };

} // namespace svc::bridge
