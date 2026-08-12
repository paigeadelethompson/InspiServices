// InspiServices - cryptography helpers (OpenSSL backend).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "services/util/util.h"

namespace svc::crypto {

  // ---- base64 -----------------------------------------------------------
  [[nodiscard]] std::string base64_encode(std::string_view data);
  [[nodiscard]] std::string
  base64_decode(std::string_view text); // raw bytes, may be binary
  [[nodiscard]] bool timing_safe_equal(std::string_view a, std::string_view b);

  // ---- hashes -----------------------------------------------------------
  [[nodiscard]] std::string sha256_hex(std::string_view data);
  [[nodiscard]] std::string hmac_sha256_hex(std::string_view key,
                                            std::string_view message);
  // Raw (binary) HMAC-SHA256 of `message` keyed by `key`, as unencoded bytes.
  [[nodiscard]] std::string hmac_sha256(std::string_view key,
                                        std::string_view message);

  // PBKDF2-HMAC-SHA256 with 256-bit derived key. 'salt' should be raw bytes.
  [[nodiscard]] std::string pbkdf2_sha256(std::string_view password,
                                          std::string_view salt,
                                          std::uint32_t iterations);
  [[nodiscard]] std::string random_bytes(std::size_t n);
  [[nodiscard]] std::string random_hex(std::size_t bytes);
  [[nodiscard]] std::string random_charset(std::size_t len,
                                           std::string_view charset);

} // namespace svc::crypto