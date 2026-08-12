// InspiServices - a single link to an InspIRCd spanning-tree server.
//
// Handles one server-to-server connection (either an outbound link we've
// dialled to an InspIRCd hub, or a connection accepted from a bridge). The
// object owns a BufferedStream and drives the InspIRCd server protocol state
// machine (CAPAB -> SERVER/password -> BURST/ENDBURST -> live).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "services/irc/protocol.h"
#include "services/net/socket.h"

namespace svc::irc {

  struct link_config {
    std::string server_name; // our server name presented on this link
    std::string server_sid;  // our server id presented on this link
    std::string server_desc;
    std::string send_pass; // password we present
    std::string recv_pass; // password we expect
    std::string host;      // outbound connect target (tcp:port)
    std::string port;
    bool send_tls = false; // TLS on this link
    bool accept = false;   // inbound (adopted socket)
    bool reconnect = false; // auto-reconnect the outbound link when it drops
    std::chrono::milliseconds retry_min{5000};  // initial backoff delay
    std::chrono::milliseconds retry_max{300000}; // backoff cap
  };

  enum class link_state {
    idle,
    connecting, // outbound TCP/TLS in progress
    nego,       // CAPAB negotiation
    waiting,    // SERVER exchanged, awaiting peer's burst
    burst,      // peer is bursting to us
    linked,     // fully linked
    dying,
  };

  class link {
  public:
    link(net::Reactor &reactor, link_config cfg);
    link(link const &) = delete;
    link &operator=(link const &) = delete;
    ~link();

    // Outbound connection. Returns false on immediate failure.
    bool connect();
    // Inbound: adopt an already-connecting socket.
    void adopt(int fd, std::shared_ptr<net::tls_session> tls);
    void close(std::string_view reason = {});

    bool send(message const &msg);
    bool send_line(std::string_view line);

    // Writes `BURST <now_stream>`, all lines produced by on_burst, then
    // ENDBURST. Idempotent.
    void send_burst();

    // ---- state / info
    [[nodiscard]] link_state state() const noexcept { return state_; }
    [[nodiscard]] bool linked() const noexcept {
      return state_ == link_state::linked;
    }
    [[nodiscard]] bool is_inbound() const noexcept { return inbound_; }
    [[nodiscard]] std::string const &remote_name() const noexcept {
      return remote_name_;
    }
    [[nodiscard]] std::string const &remote_sid() const noexcept {
      return remote_sid_;
    }
    [[nodiscard]] net::Reactor &reactor() noexcept { return reactor_; }
    [[nodiscard]] link_config const &config() const noexcept { return cfg_; }

    // Fired once the peer's identity is known (name passed).
    std::function<void(link &, std::string_view)> on_remote_id;
    // Fired for every authenticated message received.
    std::function<void(link &, message const &)> on_line;
    // Supplies the contents of our netburst (fired inside send_burst).
    std::function<void(link &)> on_burst;
    // Fired when the link becomes fully live (ENDBURST exchanged).
    std::function<void(link &)> on_link;
    // Fired when the transport is destroyed for any reason.
    std::function<void(link &)> on_close;
    // Fired when a reconnecting uplink's transport comes back up (lets the
    // owner re-register the link as active).
    std::function<void(link &)> on_reconnect;

  private:
    void attach();
    void handle_line(std::string_view line);
    void handle_capab(message const &);
    void handle_ping(message const &);
    void on_transport_ready();
    void send_capabilities();
    bool handle_server(message const &);
    void ensure_burst();
    void send_server_line();
    void to_linked();
    void reset_session();
    void cancel_retry();
    void schedule_reconnect();
    void reconnect_once();

    net::BufferedStream stream_;
    net::Reactor &reactor_;
    link_config cfg_;
    link_state state_ = link_state::idle;
    bool inbound_ = false;
    std::string remote_name_;
    std::string remote_sid_;
    unsigned proto_ = 1206;
    std::string our_challenge_;
    std::string their_challenge_;
    std::string rx_; // partial line buffer
    bool caps_done_ = false;
    bool server_sent_ = false;
    bool burst_sent_ = false;
    net::Reactor::Handle retry_timer_ = net::Reactor::bad_handle;
    unsigned attempts_ = 0;
  };

} // namespace svc::irc