// AnswerServices - native Discord bridge.
//
// Connects to the Discord REST API and Gateway over a WebSocket, authenticates
// as a bot, and relays messages between Discord channels and IRC channels using
// a per-bridge mapping table, all in-process (no external bot process needed).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "services/net/eventloop.h"
#include "services/net/http.h"
#include "services/net/socket.h"
#include "services/net/websocket.h"
#include <json/json.h>

namespace svc::bridge {

  // A single Discord bot relay. One instance per `discord` bridge row.
  class discord_bridge {
  public:
    // host: gateway endpoint host (usually "gateway.discord.gg").
    // token: bot token from the environment (never persisted in the DB).
    discord_bridge(net::Reactor &reactor, std::string host, std::string token);
    ~discord_bridge();

    discord_bridge(discord_bridge const &) = delete;
    discord_bridge &operator=(discord_bridge const &) = delete;

    // Starts connecting (call once; reconnect is automatic).
    void start();

    // Sends <text> to the Discord channel <channel_id> via REST.
    void send_message(std::string_view channel_id, std::string_view text);

    // Calling convention: (channel_id, author_display, content).
    std::function<void(std::string_view, std::string_view, std::string_view)>
        on_message;
    // Notification that the gateway reached READY (boolean = connected).
    std::function<void(bool)> on_state;

  private:
    void connect_gateway(std::string const &url);
    void on_ws_text(std::span<const char> data);
    void send_identify();
    void heartbeat();
    void schedule_reconnect();

    std::string host_;
    std::string token_;
    net::Reactor &reactor_;
    net::WebSocket ws_;
    net::Reactor::Handle heartbeat_timer_ = net::Reactor::bad_handle;
    net::Reactor::Handle reconnect_timer_ = net::Reactor::bad_handle;
    int last_seq_ = 0;
    bool identified_ = false;
    bool ready_ = false;
    std::string session_id_;
    unsigned backoff_ = 1; // seconds, doubles up to a cap
  };

  // REST payload for creating a Discord message.
  std::string discord_body(std::string_view content);

} // namespace svc::bridge