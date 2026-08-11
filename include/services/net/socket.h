// AnswerServices - TCP socket layer.
//
// Non-blocking sockets driven on non-blocking Reactor. Building blocks:
//   * TcpListener    - passive accept loop.
//   * BufferedStream - read/write buffering over a connected fd, optionally
//                      tunneled through TLS.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "services/net/eventloop.h"

typedef struct ssl_st SSL;

namespace svc::net {

  class tls_session;

  enum class transport_state {
    connecting,  // TCP connect in progress
    handshaking, // TLS handshake in progress
    open,
    closed,
    error,
  };

  class BufferedStream {
  public:
    using reader = std::function<void(std::span<const char>)>;
    using event = std::function<void()>;

    BufferedStream() = default;
    ~BufferedStream();

    BufferedStream(const BufferedStream &) = delete;
    BufferedStream &operator=(const BufferedStream &) = delete;

    // Non-blocking outbound connection. When `tls` is true the stream is
    // wrapped in a client TLS session (verify/SNI from tls_settings()).
    bool connect(Reactor &reactor, std::string_view host, std::string_view port,
                 bool tls);

    // Takes ownership of an accepted fd; `tls_ctx` enables server TLS.
    void adopt(Reactor &reactor, int fd,
               std::shared_ptr<tls_session> tls_ctx = {});

    bool send(std::string_view data);
    void close();

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] transport_state state() const noexcept { return state_; }
    [[nodiscard]] bool is_open() const noexcept {
      return state_ == transport_state::open;
    }
    [[nodiscard]] std::string const &error() const noexcept { return error_; }
    [[nodiscard]] bool tls_active() const noexcept {
      return static_cast<bool>(ssl_);
    }

    // Callbacks.
    reader on_data;     // decoded plaintext
    event on_open;      // raw TCP established (or TLS session ready in non-TLS)
    event on_tls_ready; // TLS handshake finished
    event on_close;     // remote closed / fatal error
    event on_writable;

  private:
    void on_socket_readable();
    void on_socket_writable();
    void on_socket_hangup();
    void check_connect_result();
    void pump_tls_handshake();
    void pump_tls_read();
    void drain_tls_out();
    void pump_write();
    void handle_close();

    Reactor *reactor_ = nullptr;
    Reactor::Handle rh_ = Reactor::bad_handle;
    int fd_ = -1;
    std::string rx_; // decrypted bytes awaiting consumer read
    std::string tx_; // plaintext pending SSL_write / raw socket
    std::shared_ptr<tls_session> tls_ctx_;
    SSL *ssl_ = nullptr;
    transport_state state_ = transport_state::connecting;
    std::string error_;
    bool tls_write_want_ = false;
  };

  // Passive server socket.
  class TcpListener {
  public:
    using accept_handler = std::function<void(int clientfd)>;

    TcpListener() = default;
    ~TcpListener();

    bool listen(Reactor &reactor, std::string_view bind, std::uint16_t port);
    void close();
    [[nodiscard]] int fd() const noexcept { return fd_; }

    std::function<void(int)> on_accept;
    std::function<void()> on_error;

  private:
    void on_accept_event();

    Reactor *reactor_ = nullptr;
    Reactor::Handle rh_ = Reactor::bad_handle;
    int fd_ = -1;
  };

} // namespace svc::net