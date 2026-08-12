#include "services/net/websocket.h"

#include <cstdlib>
#include <cstring>
#include <istream>
#include <random>
#include <sstream>

#include "services/net/tls.h"
#include "services/util/crypto.h"
#include "services/util/util.h"

namespace svc::net {

  namespace {

    void append_frame(std::string &out, unsigned char opcode,
                      std::span<const char> payload) {
      // Client->server frames must be masked.
      out.push_back(static_cast<char>(0x80 | opcode));
      if (payload.size() < 126)
        out.push_back(static_cast<char>(0x80 | payload.size()));
      else if (payload.size() <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        out.push_back(static_cast<char>(payload.size() & 0xFF));
      } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
          out.push_back(static_cast<char>(
              (static_cast<std::uint64_t>(payload.size()) >> (i * 8)) & 0xFF));
      }

      unsigned char mask[4];
      for (unsigned char &m : mask)
        m = static_cast<unsigned char>(::rand() % 256);
      out.append(reinterpret_cast<char *>(mask), 4);
      for (std::size_t i = 0; i < payload.size(); ++i)
        out.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                        mask[i % 4]));
    }

  } // namespace

  bool WebSocket::connect(
      Reactor &reactor, std::string url,
      std::vector<std::pair<std::string, std::string>> extra_headers) {
    if (!sv::starts_with(url, "ws://") && !sv::starts_with(url, "wss://"))
      return false;
    reactor_ = &reactor;
    url_ = std::move(url);

    // ws://host:port/path
    std::string rest = url_.substr(url_.find("://") + 3);
    std::string::size_type const slash = rest.find('/');
    std::string const hostport =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string const path =
        slash == std::string::npos ? "/" : rest.substr(slash);
    bool const tls = sv::starts_with(url_, "wss://");

    std::string host = hostport;
    std::string port = tls ? "443" : "80";
    std::size_t const colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(':') == colon) {
      host = hostport.substr(0, colon);
      port = hostport.substr(colon + 1);
    }

    std::string const key =
        svc::crypto::base64_encode(svc::crypto::random_bytes(16));

    std::string request;
    request += "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "User-Agent: InspiServices/0.1\r\n";
    for (auto const &[k, v] : extra_headers)
      request += k + ": " + v + "\r\n";
    request += "\r\n";

    stream_.on_open = [this, request]() { stream_.send(request); };
    stream_.on_close = [this] { on_stream_close(); };
    stream_.on_data = [this](std::span<const char> data) {
      recv_buffer_.append(data.begin(), data.end());
      parse_stream();
    };

    return stream_.connect(reactor, host, port, tls);
  }

  void WebSocket::parse_stream() {
    // Fast handshake response check.
    if (!connected_) {
      std::string::size_type const sep = recv_buffer_.find("\r\n\r\n");
      if (sep == std::string::npos) {
        if (recv_buffer_.size() > 65536 && on_error)
          on_error("websocket handshake overflow");
        return;
      }
      std::string const head = recv_buffer_.substr(0, sep);
      recv_buffer_.erase(0, sep + 4);

      bool ok = false;
      std::size_t const nl = head.find("\r\n");
      if (nl != std::string::npos) {
        std::istringstream ls(head.substr(0, nl));
        std::string proto, code;
        ls >> proto >> code;
        ok = code == "101";
      }
      if (!ok) {
        if (on_error)
          on_error("websocket handshake rejected");
        close();
        return;
      }
      connected_ = true;
      if (on_open)
        on_open();
    }

    // Frame parsing loop.
    while (true) {
      if (recv_buffer_.size() < 2)
        return;
      unsigned char const b0 = static_cast<unsigned char>(recv_buffer_[0]);
      unsigned char const b1 = static_cast<unsigned char>(recv_buffer_[1]);
      unsigned char const opcode = b0 & 0x0F;
      bool const fin = (b0 & 0x80) != 0;
      bool const masked = (b1 & 0x80) != 0;
      std::uint64_t len = b1 & 0x7F;
      std::size_t header = 2;
      if (len == 126) {
        if (recv_buffer_.size() < 4)
          return;
        len = (static_cast<std::uint64_t>(
                   static_cast<unsigned char>(recv_buffer_[2]))
               << 8) |
              static_cast<std::uint64_t>(
                  static_cast<unsigned char>(recv_buffer_[3]));
        header = 4;
      } else if (len == 127) {
        if (recv_buffer_.size() < 10)
          return;
        len = 0;
        for (std::size_t i = 0; i < 8; ++i)
          len = (len << 8) | static_cast<unsigned char>(recv_buffer_[2 + i]);
        header = 10;
      }

      unsigned char mask[4] = {0, 0, 0, 0};
      if (masked) {
        if (recv_buffer_.size() < header + 4)
          return;
        std::memcpy(mask, recv_buffer_.data() + header, 4);
        header += 4;
      }
      if (len > (1u << 20)) {
        if (on_error)
          on_error("websocket frame too large");
        close();
        return;
      }
      if (recv_buffer_.size() < header + len)
        return;

      std::string payload(recv_buffer_, header, static_cast<std::size_t>(len));
      if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i)
          payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
      }
      recv_buffer_.erase(0, header + static_cast<std::size_t>(len));

      if (fin && opcode == 0x08) // close
      {
        connected_ = false;
        if (on_close)
          on_close();
        return;
      }
      if (opcode == 0x09) // ping -> pong
      {
        std::string resp = "";
        append_frame(resp, 0x0A, payload);
        stream_.send(resp);
        continue;
      }
      if (opcode == 0x0A) // pong, ignore
        continue;
      if (opcode == 0x1 || opcode == 0x2) {
        if (opcode == 0x1 && on_text)
          on_text(payload);
        if (opcode == 0x2 && on_binary)
          on_binary(payload);
      }
    }
  }

  void WebSocket::on_stream_readable() {}
  void WebSocket::on_stream_close() {
    connected_ = false;
    if (on_close)
      on_close();
  }

  bool WebSocket::send_text(std::string const &text) {
    if (!connected_)
      return false;
    std::string frame;
    append_frame(frame, 0x1, text);
    return stream_.send(frame);
  }

  bool WebSocket::send_binary(std::span<const char> data) {
    if (!connected_)
      return false;
    std::string frame;
    append_frame(frame, 0x2, data);
    return stream_.send(frame);
  }

  // Closes the websocket connection (best effort per RFC 6455).
  void WebSocket::close() {
    if (connected_) {
      std::string frame;
      append_frame(frame, 0x8, "\x03\xe8"); // 1000 normal closure
      stream_.send(frame);
    }
    connected_ = false;
    stream_.close();
  }

} // namespace svc::net