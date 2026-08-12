#include "services/irc/link.h"

#include <algorithm>
#include <cctype>

#include "services/irc/protocol.h"
#include "services/irc/routing.h"
#include "services/util/crypto.h"
#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::irc {
  namespace {

    std::string make_pass(std::string_view pass, std::string_view challenge) {
      if (challenge.empty())
        return std::string(pass);
      // AUTH:<base64(hmac_sha256(pass, challenge))>
      // InspIRCd's Base64::Encode drops trailing '=' padding, so we must too or
      // the link is rejected as a password mismatch.
      std::string b64 =
          svc::crypto::base64_encode(svc::crypto::hmac_sha256(pass, challenge));
      while (!b64.empty() && b64.back() == '=')
        b64.pop_back();
      return "AUTH:" + b64;
    }

    // Compares a presented password to the expected plaintext and, if challenge
    // auth is in use, the expected hashed form.
    bool pass_matches(std::string_view presented, std::string_view plain,
                      std::string_view challenge) {
      if (svc::crypto::timing_safe_equal(presented, plain))
        return true;
      if (!challenge.empty())
        return svc::crypto::timing_safe_equal(presented,
                                              make_pass(plain, challenge));
      return false;
    }

  } // namespace

  link::link(net::Reactor &reactor, link_config cfg)
      : reactor_(reactor), cfg_(std::move(cfg)) {}

  link::~link() {
    cancel_retry();
    stream_.on_data = {};
    stream_.on_open = {};
    stream_.on_tls_ready = {};
    stream_.on_close = {};
    stream_.close();
  }

  void link::attach() {
    // The sniffing of the transport ready state differs for a freshly created
    // TLS session vs a plain connection, so we always rely on these callbacks.
    stream_.on_tls_ready = [this]() { on_transport_ready(); };
    stream_.on_close = [this]() {
      bool const dropped = (state_ != link_state::dying);
      if (dropped) {
        std::string const err = stream_.error();
        svc::log::warn("irc", "link transport closed unexpectedly ({}:{}): {}",
                       cfg_.host, cfg_.port,
                       err.empty() ? "peer closed (FIN)" : err);
        stream_.close(); // reclaim the fd
      }
      state_ = link_state::dying;
      if (dropped) {
        if (on_close)
          on_close(*this);
        schedule_reconnect();
      }
    };
    stream_.on_data = [this](std::span<char const> data) {
      rx_.append(data.begin(), data.end());
      std::size_t nl = 0;
      while ((nl = rx_.find('\n')) != std::string::npos) {
        std::string line = rx_.substr(0, nl);
        rx_.erase(0, nl + 1);
        handle_line(line);
        if (state_ == link_state::dying)
          return;
      }
    };
  }

  bool link::connect() {
    reset_session();
    state_ = link_state::connecting;
    attach();
    bool const ok =
        stream_.connect(reactor_, cfg_.host, cfg_.port, cfg_.send_tls);
    if (!ok) {
      svc::log::error("irc", "link to {}:{} (tls={}) failed to connect: {}",
                      cfg_.host, cfg_.port, cfg_.send_tls, stream_.error());
      schedule_reconnect();
      return false;
    }
    svc::log::info("irc", "connecting to {}:{} (tls={})", cfg_.host, cfg_.port,
                   cfg_.send_tls);
    return true;
  }

  void link::adopt(int fd, std::shared_ptr<net::tls_session> tls) {
    inbound_ = true;
    state_ = link_state::nego;
    attach();
    stream_.adopt(reactor_, fd, std::move(tls));
    on_transport_ready();
  }

  void link::on_transport_ready() {
    if (state_ != link_state::connecting && state_ != link_state::nego)
      return;
    state_ = link_state::nego;
    caps_done_ = false;
    our_challenge_ = svc::crypto::random_hex(20);
    if (on_reconnect)
      on_reconnect(*this);
    svc::log::info("irc", "link transport ready ({}:{}), starting CAPAB",
                   cfg_.host, cfg_.port);
    send_line("CAPAB START 1206");
  }

  void link::send_capabilities() {
    if (caps_done_)
      return;
    caps_done_ = true;
    // We advertise our challenge to enable hashed authentication.
    svc::log::info("irc", "  sending CAPAB CAPABILITIES (CHALLENGE) + END");
    send_line("CAPAB CAPABILITIES :CHALLENGE=" + our_challenge_);
    send_line("CAPAB END");
  }

  void link::send_server_line() {
    if (server_sent_)
      return;
    server_sent_ = true;
    std::string const pass = make_pass(cfg_.send_pass, their_challenge_);
    if (proto_ == 1205)
      send_line(sv::fmt("SERVER {} {} 0 {} :{}", cfg_.server_name, pass,
                        cfg_.server_sid, cfg_.server_desc));
    else
      send_line(sv::fmt("SERVER {} {} {} :{}", cfg_.server_name, pass,
                        cfg_.server_sid, cfg_.server_desc));
  }

  void link::handle_line(std::string_view line) {
    if (line.empty())
      return;
    if (line.back() == '\r')
      line.remove_suffix(1);
    if (line.empty())
      return;
    svc::log::debug("irc", "<< {}", line);

    message msg = message::parse(line);
    if (msg.empty())
      return;

    if (msg.command == "ERROR") {
      svc::log::warn("irc", "link closed by peer: {}", msg.param_or(0));
      close(msg.param_or(0, "remote error"));
      return;
    }

    switch (state_) {
    case link_state::nego:
      if (msg.command == "CAPAB")
        handle_capab(msg);
      else if (msg.command == "SERVER")
        handle_server(msg); // inbound first
      else if (msg.command == "PING" || msg.command == "PONG")
        handle_ping(msg);
      else
        svc::log::debug("irc", "ignoring '{}' in negotiation", msg.command);
      return;

    case link_state::waiting:
      if (msg.command == "SERVER")
        handle_server(msg);
      else if (msg.command == "BURST") {
        if (on_line)
          on_line(*this, msg);
        ensure_burst();
        state_ = link_state::burst;
        svc::log::debug("irc", "state: waiting -> burst");
      } else if (msg.command == "ENDBURST")
        to_linked();
      else if (msg.command == "PING" || msg.command == "PONG")
        handle_ping(msg);
      else
        svc::log::debug("irc", "unexpected '{}' in waiting", msg.command);
      return;

    case link_state::burst:
      if (msg.command == "BURST") {
        // Repeated burst header; ignore as already bursting.
        break;
      }
      if (msg.command == "ENDBURST") {
        to_linked();
        return;
      }
      [[fallthrough]];

    case link_state::linked:
      if (msg.command == "PING")
        handle_ping(msg);
      else if (on_line)
        on_line(*this, msg);
      return;

    default:
      return;
    }
  }

  void link::handle_capab(message const &msg) {
    if (msg.params.empty()) {
      close("bad CAPAB");
      return;
    }
    std::string const &sub = msg.params[0];
    if (sv::equals_ci(sub, "START")) {
      if (msg.params.size() > 1) {
        unsigned v = 1206;
        (void)sv::try_parse(msg.params[1], v);
        proto_ = std::clamp<unsigned>(v, 1203, 1206);
      }
      svc::log::info("irc", "  peer CAPAB START (protocol {})", proto_);
      send_capabilities();
    } else if (sv::equals_ci(sub, "CAPABILITIES")) {
      if (msg.params.size() >= 2) {
        for (auto const &item : sv::splitws(msg.params[1])) {
          std::size_t const eq = item.find('=');
          if (eq == std::string::npos)
            continue;
          if (item.substr(0, eq) == "CHALL" ||
              item.substr(0, eq) == "CHALLENGE")
            their_challenge_ = item.substr(eq + 1);
        }
      }
      svc::log::info("irc", "  peer CAPAB CAPABILITIES (challenge {})",
                     their_challenge_.empty() ? "none" : "received");
    } else if (sv::equals_ci(sub, "END")) {
      svc::log::info("irc", "  peer CAPAB END");
      if (!inbound_)
        send_server_line(); // we are the connecting side: introduce ourselves
    }
  }

  void link::handle_ping(message const &msg) {
    // InspIRCd server links ping with "PING <receiver-sid>" and expect a PONG
    // whose first parameter is the ping sender's own server id; that is the
    // directly connected peer, whose sid is known by now. Echo any trailing
    // parameter if present.
    if (msg.command != "PING") {
      (void)msg;
      return;
    }
    std::string const token =
        remote_sid_.empty() ? msg.param_or(0) : remote_sid_;
    if (msg.params.size() > 1)
      send_line(
          sv::fmt(":{} PONG {} {}", cfg_.server_sid, token, msg.params[1]));
    else
      send_line(sv::fmt(":{} PONG {}", cfg_.server_sid, token));
  }

  bool link::handle_server(message const &msg) {
    // SERVER introduces the peer's name/sid and proves their password.
    std::size_t const sidx = (proto_ == 1205) ? 3 : 2;
    if (msg.params.size() <= sidx) {
      close("short SERVER");
      return false;
    }
    std::string const name = msg.params[0];
    std::string const &password = msg.params[1];
    std::string const sid = msg.params[sidx];
    std::string const desc = msg.params.back();

    if (inbound_) {
      // We are the listening side: their SERVER is the first thing after the
      // capabilities. Verify and reply.
      if (!pass_matches(password, cfg_.recv_pass, our_challenge_)) {
        svc::log::warn("irc", "rejecting '{}': bad password", name);
        send_line("ERROR :Invalid server password");
        close("auth failed");
        return false;
      }
    } else {
      // We are the connecting side: this is their reply to our SERVER.
      if (!pass_matches(password, cfg_.recv_pass, our_challenge_)) {
        svc::log::warn("irc", "'{}' rejected our server password", name);
        close("auth failed");
        return false;
      }
    }

    remote_name_ = name;
    remote_sid_ = sid;
    svc::log::info("irc", "  peer SERVER '{}' (sid {}) authenticated", name,
                   sid);
    svc::log::debug("irc", "  peer descriptor: {}", desc);
    if (on_remote_id)
      on_remote_id(*this, name);

    if (inbound_) {
      send_server_line();
      state_ = link_state::waiting; // wait for their BURST
      svc::log::debug("irc", "state: nego -> waiting (inbound)");
    } else {
      // We've authenticated; transition to linked and start our burst.
      ensure_burst();
      state_ = link_state::burst;
      svc::log::debug("irc", "state: waiting -> burst (outbound)");
    }
    return true;
  }

  void link::ensure_burst() {
    if (burst_sent_)
      return;
    burst_sent_ = true;
    send_burst();
  }

  void link::send_burst() {
    svc::log::info("irc", "  opening burst to '{}'",
                   remote_name_.empty() ? cfg_.host : remote_name_);
    send_line(sv::fmt(":{} BURST {}", cfg_.server_sid,
                      std::to_string(svc::irc::now())));
    if (on_burst)
      on_burst(*this);
    send_line(":" + cfg_.server_sid + " ENDBURST");
  }

  void link::to_linked() {
    attempts_ = 0; // reset backoff on every successful link
    state_ = link_state::linked;
    svc::log::info("irc", "link with '{}' is up (linked)",
                   remote_name_.empty() ? cfg_.host : remote_name_);
    svc::log::debug("irc", "state: burst -> linked");
    if (on_link)
      on_link(*this);
  }

  bool link::send(message const &msg) { return send_line(msg.to_wire()); }

  bool link::send_line(std::string_view line) {
    if (state_ == link_state::dying)
      return false;
    // Debug lines keep the link trace readable; redact the server link
    // password (it is derived from the shared secret).
    std::string_view dbgl = line;
    std::string redacted;
    if (line.size() >= 7 && line.compare(0, 7, "SERVER ") == 0) {
      redacted = "SERVER ... (credentials redacted)";
      dbgl = redacted;
    }
    svc::log::debug("irc", ">> {}", dbgl);
    std::string out(line);
    out.push_back('\n');
    return stream_.send(out);
  }

  void link::close(std::string_view reason) {
    if (state_ == link_state::dying)
      return;
    state_ = link_state::dying;
    svc::log::info("irc", "link closed: {}", reason);
    if (on_close)
      on_close(*this);
    stream_.close();
    schedule_reconnect();
  }

  void link::reset_session() {
    // Reusable connection: drop all per-handshake state so a reconnect starts
    // from a clean CAPAB -> SERVER -> BURST exchange.
    state_ = link_state::idle;
    rx_.clear();
    remote_name_.clear();
    remote_sid_.clear();
    proto_ = 1206;
    our_challenge_.clear();
    their_challenge_.clear();
    caps_done_ = false;
    server_sent_ = false;
    burst_sent_ = false;
  }

  void link::cancel_retry() {
    if (retry_timer_ != net::Reactor::bad_handle) {
      reactor_.remove_timer(retry_timer_);
      retry_timer_ = net::Reactor::bad_handle;
    }
  }

  void link::schedule_reconnect() {
    if (!cfg_.reconnect || inbound_)
      return;
    cancel_retry();
    std::chrono::milliseconds delay = cfg_.retry_min;
    for (unsigned i = 0; i < attempts_ && delay < cfg_.retry_max; ++i)
      delay *= 2;
    if (delay > cfg_.retry_max)
      delay = cfg_.retry_max;
    ++attempts_;
    svc::log::warn("irc", "reconnecting to {}:{} in {}s (attempt {})",
                   cfg_.host, cfg_.port, delay.count() / 1000, attempts_);
    retry_timer_ = reactor_.add_timer(delay, [this] {
      retry_timer_ = net::Reactor::bad_handle;
      reconnect_once();
    });
  }

  void link::reconnect_once() {
    svc::log::info("irc", "retrying uplink to {}:{}", cfg_.host, cfg_.port);
    connect(); // failure is handled inside (reschedules)
  }

} // namespace svc::irc