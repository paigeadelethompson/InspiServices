// AnswerServices - RFC 6455 WebSocket client.
//
// Used for the Discord gateway (wss://) and for signal-cli daemons configured
// to listen on a WebSocket rather than a UNIX socket/TCP socket.
#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "services/net/socket.h"

namespace svc::net {

  class WebSocket {
  public:
    using msg_cb = std::function<void(std::span<const char>)>; // text or binary
    using event_cb = std::function<void()>;

    WebSocket() = default;

    // url: ws://host:port/path or wss://...
    bool connect(
        Reactor &reactor, std::string url,
        std::vector<std::pair<std::string, std::string>> extra_headers = {});

    // Sends a text/binary message.
    bool send_text(std::string const &text);
    bool send_binary(std::span<const char> data);

    // Sends a close frame and closes the underlying transport.
    void close();
    [[nodiscard]] bool connected() const noexcept { return connected_; }

    // Callbacks.
    event_cb on_open;
    msg_cb on_text; // text frames forwarded here
    msg_cb on_binary;
    event_cb on_close;
    std::function<void(std::string const &)> on_error;

  private:
    void on_stream_readable();
    void on_stream_close();
    void parse_stream();

    Reactor *reactor_ = nullptr;
    BufferedStream stream_;
    bool connected_ = false;
    bool closing_ = false;
    std::string recv_buffer_;
    std::string url_;
  };

} // namespace svc::net