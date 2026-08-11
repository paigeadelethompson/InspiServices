// AnswerServices - native Signal bridge.
//
// Speaks signal-cli's JSON-RPC 2.0 over a TCP socket (`signal-cli daemon
// --tcp <port>`, newline-delimited JSON-RPC). The account number and the
// daemon endpoint are configured per bridge row; tokens/registration are the
// responsibility of the locally running signal-cli daemon.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "services/net/eventloop.h"
#include "services/net/socket.h"

namespace svc::bridge {

  class signal_bridge {
  public:
    // host/port: address of the signal-cli daemon (JSON-RPC/TCP).
    // account: phone number the daemon was started with (-a).
    signal_bridge(net::Reactor &reactor, std::string host, std::string port,
                  std::string account);
    ~signal_bridge();

    signal_bridge(signal_bridge const &) = delete;
    signal_bridge &operator=(signal_bridge const &) = delete;

    void start();

    // Sends a text message to a recipient (phone number) or a group (groupId).
    // The receiver selects the target; group targets use `groupId`.
    void send_message(std::string_view recipient, std::string_view text);
    void send_group_message(std::string_view group_id, std::string_view text);

    // Callbacks.
    // (sender number or group id, display name, text)
    std::function<void(std::string_view, std::string_view, std::string_view)>
        on_message;
    std::function<void(bool)> on_state;

  private:
    void connect();
    void on_data(std::span<const char> data);
    void handle_line(std::string_view line);
    void pump_rx();
    void schedule_resume();

    net::Reactor &reactor_;
    std::string host_;
    std::string port_;
    std::string account_;
    net::BufferedStream stream_;
    std::string rx_; // partial line buffer
    std::uint64_t next_id_ = 1;
    bool connected_ = false;
    net::Reactor::Handle resume_timer_ = net::Reactor::bad_handle;
    unsigned backoff_ = 1;
  };

  // Builds the JSON-RPC "send" request used by signal_bridge::send_message.
  std::string signal_send_request(std::string_view account,
                                  std::string_view recipient,
                                  std::string_view text, bool is_group,
                                  std::uint64_t id);

} // namespace svc::bridge