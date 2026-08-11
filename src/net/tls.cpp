#include "services/net/tls.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <mutex>
#include <vector>

#include "services/util/util.h"

namespace svc::net {

  namespace {

    tls_config g_config;
    std::mutex g_mutex;
  } // namespace

  tls_session::~tls_session() = default;

  void tls_session::ctx_deleter::operator()(void *p) const {
    if (p)
      SSL_CTX_free(static_cast<SSL_CTX *>(p));
  }

  bool tls_session::init(const tls_config &cfg) {
    SSL_CTX *raw = SSL_CTX_new(TLS_client_method());
    if (!raw) {
      error_ = "SSL_CTX_new failed";
      return false;
    }
    ctx_.reset(raw);

    SSL_CTX *ctx = static_cast<SSL_CTX *>(ctx_.get());
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                              SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (!cfg.certfile.empty()) {
      if (SSL_CTX_use_certificate_chain_file(ctx, cfg.certfile.c_str()) != 1) {
        error_ = "could not load client certificate";
        return false;
      }
      if (!cfg.keyfile.empty() &&
          SSL_CTX_use_PrivateKey_file(ctx, cfg.keyfile.c_str(),
                                      SSL_FILETYPE_PEM) != 1) {
        error_ = "could not load client key";
        return false;
      }
      SSL_CTX_check_private_key(ctx);
    }

    if (!cfg.cafile.empty()) {
      if (SSL_CTX_load_verify_locations(ctx, cfg.cafile.c_str(), nullptr) !=
          1) {
        error_ = "could not load CA bundle";
        return false;
      }
    } else {
      SSL_CTX_set_default_verify_paths(ctx);
    }
    return true;
  }

  SSL *tls_session::new_ssl(int fd) {
    (void)fd;
    SSL *s = SSL_new(static_cast<SSL_CTX *>(ctx_.get()));
    if (!s) {
      error_ = "SSL_new failed";
      return nullptr;
    }
    return s;
  }

  // ---------------------------------------------------------------------------
  // Global client TLS context
  // ---------------------------------------------------------------------------
  namespace {
    tls_config g_settings;
    std::unique_ptr<tls_session> g_client;
    std::once_flag g_once;
  } // namespace

  tls_config const &tls_settings() { return g_settings; }

  void set_tls_config(const tls_config &cfg) { g_settings = cfg; }

  tls_session &client_tls() {
    std::call_once(g_once, [] {
      g_client = std::make_unique<tls_session>();
      if (!g_client->init(g_settings)) {
        g_client.reset();
      }
    });
    static tls_session fallback;
    return g_client ? *g_client : fallback;
  }

  tls_config parse_tls_config(std::string_view value) {
    tls_config cfg;
    for (auto const &tok : sv::split(std::string(value), ',')) {
      if (tok.empty())
        continue;
      auto const eq = tok.find('=');
      std::string const key = eq == std::string::npos ? tok : tok.substr(0, eq);
      std::string const val = eq == std::string::npos ? "" : tok.substr(eq + 1);
      if (key == "verify")
        cfg.verify = val == "1" || val == "true";
      else if (key == "cert")
        cfg.certfile = val;
      else if (key == "key")
        cfg.keyfile = val;
      else if (key == "ca")
        cfg.cafile = val;
    }
    return cfg;
  }

} // namespace svc::net