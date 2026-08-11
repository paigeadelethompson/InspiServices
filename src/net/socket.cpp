#include "services/net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "services/net/tls.h"
#include "services/util/log.h"

namespace svc::net {

  namespace {

    bool set_nonblocking(int fd) {
      int const flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0)
        return false;
      return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    int make_socket(int family) {
      int const fd = ::socket(family, SOCK_STREAM, 0);
      if (fd < 0)
        return -1;
      if (!set_nonblocking(fd)) {
        ::close(fd);
        return -1;
      }
      int const one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      return fd;
    }

    int resolve(std::string_view host, std::string_view port,
                sockaddr_storage &out, socklen_t &len) {
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      std::string h(host);
      // Shell out `[2001:db8::1]` IPv6 literals: getaddrinfo wants the bare
      // address, not the bracketed form.
      if (h.size() >= 2 && h.front() == '[' && h.back() == ']')
        h = h.substr(1, h.size() - 2);
      std::string const p(port);
      addrinfo *res = nullptr;
      int const rc = getaddrinfo(h.empty() ? nullptr : h.c_str(),
                                 p.empty() ? nullptr : p.c_str(), &hints, &res);
      if (rc != 0)
        return rc;
      if (!res)
        return EAI_NONAME;
      std::size_t const cn =
          std::min<std::size_t>(res->ai_addrlen, sizeof(out));
      std::memcpy(&out, res->ai_addr, cn);
      len = static_cast<socklen_t>(cn);
      freeaddrinfo(res);
      return 0;
    }

    void interest(Reactor *r, Reactor::Handle rh, bool read, bool write) {
      if (r && rh != Reactor::bad_handle)
        r->set_interest(rh, read, write);
    }

  } // namespace

  // ===========================================================================
  // TcpListener
  // ===========================================================================
  TcpListener::~TcpListener() { close(); }

  bool TcpListener::listen(Reactor &reactor, std::string_view bind,
                           std::uint16_t port) {
    if (bind.empty())
      bind = "0.0.0.0";

    sockaddr_storage addr{};
    socklen_t len = 0;
    if (resolve(bind, std::to_string(port), addr, len) != 0)
      return false;
    int const fd = make_socket(addr.ss_family);
    if (fd < 0)
      return false;
    int const one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), len) != 0 ||
        ::listen(fd, SOMAXCONN) != 0) {
      ::close(fd);
      return false;
    }

    fd_ = fd;
    reactor_ = &reactor;
    rh_ = reactor.add_socket(
        fd_, [this] { on_accept_event(); }, {}, [this] { close(); });
    interest(reactor_, rh_, true, false);
    return true;
  }

  void TcpListener::close() {
    if (rh_ != Reactor::bad_handle && reactor_) {
      reactor_->remove_socket(rh_);
      rh_ = Reactor::bad_handle;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void TcpListener::on_accept_event() {
    for (;;) {
      sockaddr_storage client{};
      socklen_t len = sizeof(client);
      int const cfd =
          ::accept(fd_, reinterpret_cast<sockaddr *>(&client), &len);
      if (cfd < 0) {
        if (errno == EINTR)
          continue;
        return;
      }
      set_nonblocking(cfd);
      int const one = 1;
      setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (on_accept)
        on_accept(cfd);
    }
  }

  // ===========================================================================
  // BufferedStream
  // ===========================================================================
  BufferedStream::~BufferedStream() {
    close();
    if (ssl_)
      SSL_free(ssl_);
  }

  bool BufferedStream::connect(Reactor &reactor, std::string_view host,
                               std::string_view port, bool tls) {
    sockaddr_storage addr{};
    socklen_t len = 0;
    if (resolve(host, port, addr, len) != 0) {
      error_ = "DNS resolution failed";
      return false;
    }
    int const fd = make_socket(addr.ss_family);
    if (fd < 0) {
      error_ = "socket() failed";
      return false;
    }

    if (tls) {
      tls_config cfg = tls_settings();
      cfg.server_name = std::string(host);
      SSL *s = client_tls().new_ssl(fd);
      if (!s) {
        error_ = client_tls().error();
        ::close(fd);
        return false;
      }
      if (cfg.verify) {
        SSL_set_tlsext_host_name(s, cfg.server_name.c_str());
        SSL_set1_host(s, cfg.server_name.c_str());
        SSL_set_verify(s, SSL_VERIFY_PEER, nullptr);
      } else if (!cfg.server_name.empty()) {
        SSL_set_tlsext_host_name(s, cfg.server_name.c_str());
      }
      BIO *rbio = BIO_new(BIO_s_mem());
      BIO *wbio = BIO_new(BIO_s_mem());
      if (!rbio || !wbio) {
        error_ = "TLS BIO allocation failed";
        ::close(fd);
        return false;
      }
      SSL_set_bio(s, rbio, wbio);
      SSL_set_connect_state(s);
      ssl_ = s;
    }

    fd_ = fd;
    state_ = transport_state::connecting;
    reactor_ = &reactor;

    int const rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), len);
    if (rc != 0 && errno != EINPROGRESS) {
      error_ = std::strerror(errno);
      ::close(fd);
      fd_ = -1;
      state_ = transport_state::error;
      return false;
    }

    rh_ = reactor.add_socket(
        fd_, [this] { on_socket_readable(); }, [this] { on_socket_writable(); },
        [this] { on_socket_hangup(); });
    interest(reactor_, rh_, true, true);
    return true;
  }

  void BufferedStream::adopt(Reactor &reactor, int fd,
                             std::shared_ptr<tls_session> tls_ctx) {
    fd_ = fd;
    reactor_ = &reactor;
    state_ = transport_state::open;
    if (tls_ctx) {
      tls_ctx_ = std::move(tls_ctx);
      ssl_ = tls_ctx_->new_ssl(fd);
      if (!ssl_) {
        error_ = tls_ctx_->error();
        ::close(fd);
        fd_ = -1;
        state_ = transport_state::error;
        return;
      }
      BIO *rbio = BIO_new(BIO_s_mem());
      BIO *wbio = BIO_new(BIO_s_mem());
      SSL_set_bio(ssl_, rbio, wbio);
      SSL_set_accept_state(ssl_);
      state_ = transport_state::handshaking;
    }

    rh_ = reactor.add_socket(
        fd_, [this] { on_socket_readable(); }, [this] { on_socket_writable(); },
        [this] { on_socket_hangup(); });
    if (state_ == transport_state::handshaking)
      interest(reactor_, rh_, true, true);
    else {
      interest(reactor_, rh_, true, false);
      if (on_open)
        on_open();
    }
    if (on_writable)
      on_writable();
  }

  bool BufferedStream::send(std::string_view data) {
    if (state_ != transport_state::open &&
        state_ != transport_state::handshaking)
      return false;
    tx_.append(data);
    if (state_ == transport_state::open)
      interest(reactor_, rh_, true, true);
    return true;
  }

  void BufferedStream::pump_write() {
    while (!tx_.empty()) {
      ssize_t const n = ::write(fd_, tx_.data(), tx_.size());
      if (n > 0) {
        tx_.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                     errno != EINTR)) {
        error_ = (n == 0) ? "peer shut down write side (EOF)"
                          : std::strerror(errno);
        handle_close();
        return;
      }
      break;
    }
    interest(reactor_, rh_, true, !tx_.empty());
  }

  void BufferedStream::drain_tls_out() {
    if (!ssl_)
      return;
    bool done = false;
    BIO *out = SSL_get_wbio(ssl_);
    while (!done) {
      char buf[16384];
      int const n = BIO_read(out, buf, static_cast<int>(sizeof(buf)));
      if (n <= 0)
        break;
      // The socket is non-blocking; a partial write is unlikely but possible.
      ssize_t const w = ::write(fd_, buf, static_cast<std::size_t>(n));
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        done = true;
      else if (w != n)
        done = true;
    }
  }

  void BufferedStream::pump_tls_handshake() {
    if (state_ != transport_state::handshaking || !ssl_)
      return;
    int rc = SSL_is_init_finished(ssl_);
    if (!rc)
      rc = SSL_is_server(ssl_) ? SSL_accept(ssl_) : SSL_connect(ssl_);
    if (rc == 1) {
      state_ = transport_state::open;
      drain_tls_out();
      if (on_tls_ready)
        on_tls_ready();
      if (on_writable)
        on_writable();
      return;
    }
    int const err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      drain_tls_out();
      bool const want_write = err == SSL_ERROR_WANT_WRITE;
      // If the TLS layer is waiting on the peer's reply (WANT_READ) we MUST
      // keep reading, or the handshake stalls forever with an idle socket.
      interest(reactor_, rh_, !want_write || is_open(),
               want_write || !tx_.empty());
      return;
    }
    error_ = "TLS handshake failed";
    handle_close();
  }

  void BufferedStream::pump_tls_read() {
    while (ssl_ && state_ != transport_state::closed) {
      if (state_ == transport_state::handshaking)
        pump_tls_handshake();
      if (state_ != transport_state::open)
        return;
      char inbuf[4096];
      int const ret = SSL_read(ssl_, inbuf, static_cast<int>(sizeof(inbuf)));
      if (ret > 0) {
        rx_.append(inbuf, static_cast<std::size_t>(ret));
        if (on_data)
          on_data(std::span<const char>(rx_.data(), rx_.size()));
        rx_.clear();
        continue;
      }
      int const err = SSL_get_error(ssl_, ret);
      if (err == SSL_ERROR_WANT_READ)
        return;
      if (err == SSL_ERROR_WANT_WRITE) {
        drain_tls_out();
        return;
      }
      if (err == SSL_ERROR_ZERO_RETURN) {
        handle_close();
        return;
      }
      error_ = "TLS read failure";
      handle_close();
      return;
    }
  }

  void BufferedStream::on_socket_readable() {
    if (state_ == transport_state::connecting) {
      check_connect_result();
      return;
    }

    char buf[65536];
    ssize_t const n = ::read(fd_, buf, sizeof(buf));
    if (n > 0) {
      if (!ssl_) {
        if (on_data)
          on_data(std::span(buf, static_cast<std::size_t>(n)));
        else {
          rx_.append(buf, static_cast<std::size_t>(n));
          if (rx_.size() > (1u << 20)) {
            error_ = "receive queue overflow";
            handle_close();
          }
        }
      } else {
        BIO_write(SSL_get_rbio(ssl_), buf, static_cast<int>(n));
        pump_tls_read();
      }
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                          errno != EINTR)) {
      error_ = (n == 0) ? "peer closed connection (FIN)"
                        : std::strerror(errno);
      handle_close();
    }
  }

  void BufferedStream::on_socket_writable() {
    if (state_ == transport_state::connecting) {
      check_connect_result();
      return;
    }
    if (state_ == transport_state::handshaking) {
      pump_tls_handshake();
      if (state_ != transport_state::open)
        return;
    }
    if (ssl_) {
      drain_tls_out();
      while (!tx_.empty() && ssl_) {
        int const n = SSL_write(ssl_, tx_.data(), static_cast<int>(tx_.size()));
        if (n > 0) {
          tx_.erase(0, static_cast<std::size_t>(n));
          drain_tls_out();
          continue;
        }
        int const err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
          break;
        handle_close();
        return;
      }
      interest(reactor_, rh_, true, !tx_.empty());
    } else {
      pump_write();
    }
    if (on_writable)
      on_writable();
  }

  void BufferedStream::check_connect_result() {
    int err = 0;
    socklen_t sl = sizeof(err);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &sl) != 0 || err != 0) {
      error_ = err ? std::strerror(err) : "connect() failed";
      state_ = transport_state::error;
      handle_close();
      return;
    }
    if (ssl_) {
      state_ = transport_state::handshaking;
      pump_tls_handshake();
    } else {
      state_ = transport_state::open;
      if (on_open)
        on_open();
      if (on_tls_ready)
        on_tls_ready();
    }
  }

  void BufferedStream::on_socket_hangup() { handle_close(); }

  void BufferedStream::handle_close() {
    if (state_ == transport_state::closed || state_ == transport_state::error)
      return;
    state_ = transport_state::closed;
    if (reactor_ && rh_ != Reactor::bad_handle)
      reactor_->remove_socket(rh_);
    rh_ = Reactor::bad_handle;
    if (on_close)
      on_close();
    interest(reactor_, rh_, false, false);
    (void)reactor_;
    (void)rh_; // closed socket; ignore
  }

  void BufferedStream::close() {
    handle_close();
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

} // namespace svc::net