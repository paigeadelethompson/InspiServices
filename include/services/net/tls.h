// InspiServices - TLS support (OpenSSL).
#pragma once

#include <memory>
#include <string>

#include "services/util/util.h"

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace svc::net {

  enum class tls_mode {
    client,
    server,
  };

  struct tls_config {
    bool enabled = false;
    bool verify = false;     // verify server certificates (client mode)
    std::string certfile;    // server/client certificate (PEM)
    std::string keyfile;     // private key (PEM)
    std::string cafile;      // CA bundle for verification
    std::string server_name; // SNI + hostname verification in client mode
  };

  // Holds SSL_CTX state. BufferedStream keeps a shared_ptr to it so the
  // context outlives the connection.
  class tls_session {
  public:
    tls_session() = default;
    ~tls_session();

    tls_session(const tls_session &) = delete;
    tls_session &operator=(const tls_session &) = delete;

    bool init(const tls_config &cfg);

    SSL *new_ssl(int fd);
    std::string const &error() const noexcept { return error_; }
    operator bool() const noexcept { return static_cast<bool>(ctx_); }

  private:
    // Deleter is defined in tls.cpp where the type is complete; crafted so the
    // incomplete-type unique_ptr compiles without OpenSSL public structs.
    struct ctx_deleter {
      void operator()(void *) const;
    };
    std::unique_ptr<void, ctx_deleter> ctx_;
    std::string error_;
  };

  // Shared default context for outbound client TLS connections (used by
  // HTTPS/WebSocket connections to Discord and signal-cli). Configured once
  // from the database by net::tls_init().
  tls_session &client_tls();
  void set_tls_config(const tls_config &cfg);
  tls_config const &tls_settings();

  // Convenience: parse a comma separated "verify=1,cafile=..." config string.
  tls_config parse_tls_config(std::string_view value);

} // namespace svc::net