#include "services/util/crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <memory>
#include <stdexcept>

namespace svc::crypto {

  namespace {
    std::string hex(std::string_view bytes) {
      static constexpr char const *hexchars = "0123456789abcdef";
      std::string out;
      out.reserve(bytes.size() * 2);
      for (unsigned char const c : bytes) {
        out.push_back(hexchars[c >> 4]);
        out.push_back(hexchars[c & 0x0F]);
      }
      return out;
    }
  } // namespace

  std::string base64_encode(std::string_view data) {
    if (data.empty())
      return {};
    std::size_t const needed = ((data.size() + 2) / 3) * 4;
    std::string out(needed, '\0');
    int const written =
        EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                        reinterpret_cast<const unsigned char *>(data.data()),
                        static_cast<int>(data.size()));
    out.resize(static_cast<std::size_t>(written));
    return out;
  }

  std::string base64_decode(std::string_view text) {
    if (text.empty())
      return {};
    std::string out;
    out.resize(text.size() + 4, '\0');
    int const written =
        EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                        reinterpret_cast<const unsigned char *>(text.data()),
                        static_cast<int>(text.size()));
    if (written < 0)
      return {};
    // EVP_DecodeBlock ignores trailing padding bytes; trim '=' padding.
    std::size_t padding = 0;
    if (!text.empty() && text.back() == '=')
      ++padding;
    if (text.size() > 1 && text[text.size() - 2] == '=')
      ++padding;
    out.resize(static_cast<std::size_t>(written) - padding);
    return out;
  }

  bool timing_safe_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
      return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
      diff |=
          static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
  }

  std::string sha256_hex(std::string_view data) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> buf{};
    SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(),
           buf.data());
    return hex({reinterpret_cast<const char *>(buf.data()), buf.size()});
  }

  std::string hmac_sha256_hex(std::string_view key, std::string_view message) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> buf{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(message.data()),
         message.size(), buf.data(), &len);
    return hex({reinterpret_cast<const char *>(buf.data()), len});
  }

  std::string hmac_sha256(std::string_view key, std::string_view message) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> buf{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(message.data()),
         message.size(), buf.data(), &len);
    return {reinterpret_cast<const char *>(buf.data()), len};
  }

  std::string pbkdf2_sha256(std::string_view password, std::string_view salt,
                            std::uint32_t iterations) {
    std::array<unsigned char, 32> buf{};
    PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                      reinterpret_cast<const unsigned char *>(salt.data()),
                      static_cast<int>(salt.size()),
                      static_cast<int>(iterations), EVP_sha256(),
                      static_cast<int>(buf.size()), buf.data());
    return {reinterpret_cast<const char *>(buf.data()), buf.size()};
  }

  std::string random_bytes(std::size_t n) {
    std::string out(n, '\0');
    if (!out.empty() &&
        RAND_bytes(reinterpret_cast<unsigned char *>(out.data()),
                   static_cast<int>(n)) != 1)
      throw std::runtime_error("RAND_bytes failed");
    return out;
  }

  std::string random_hex(std::size_t bytes) { return hex(random_bytes(bytes)); }

  std::string random_charset(std::size_t len, std::string_view charset) {
    if (charset.empty())
      return {};
    std::string bytes = random_bytes(len);
    std::string out;
    out.reserve(len);
    for (unsigned char const c : bytes)
      out.push_back(charset[c % charset.size()]);
    return out;
  }

} // namespace svc::crypto