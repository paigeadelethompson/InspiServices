#include "services/net/http.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>

#include "services/net/eventloop.h"
#include "services/net/socket.h"
#include "services/util/util.h"

namespace svc::net {

  namespace {

    struct url_parts {
      std::string scheme;
      std::string host;
      std::string port;
      std::string path;
    };

    bool parse_url(std::string const &raw, url_parts &out) {
      std::string::size_type const sch = raw.find("://");
      if (sch == std::string::npos)
        return false;
      out.scheme = raw.substr(0, sch);
      std::string const rest = raw.substr(sch + 3);
      std::string::size_type const slh = rest.find('/');
      std::string const hostport =
          slh == std::string::npos ? rest : rest.substr(0, slh);
      out.path = slh == std::string::npos ? "/" : rest.substr(slh);

      std::string::size_type const colon = hostport.rfind(':');
      if (colon != std::string::npos && hostport.find(':') == colon) {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
      } else {
        out.host = hostport;
        out.port = out.scheme == "https" ? "443" : "80";
      }
      return !out.host.empty();
    }

    std::string encode_request(HttpRequest const &req, url_parts const &u) {
      std::string out;
      out.reserve(256 + req.body.size());
      out += req.method;
      out += ' ';
      out += u.path;
      out += " HTTP/1.1\r\n";
      out += "Host: " + u.host + "\r\n";
      out += "User-Agent: InspiServices/0.1\r\n";
      out += "Accept: */*\r\n";
      bool has_ct = false;
      bool has_cl = false;
      for (auto const &[k, v] : req.headers) {
        if (sv::equals_ci(k, "Content-Type"))
          has_ct = true;
        if (sv::equals_ci(k, "Content-Length"))
          has_cl = true;
        out += k + ": " + v + "\r\n";
      }
      if (!req.body.empty() && !has_ct)
        out += "Content-Type: application/json\r\n";
      if (!req.body.empty() && !has_cl)
        out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
      out += "Connection: close\r\n\r\n";
      out += req.body;
      return out;
    }

  } // namespace

  void http_request(Reactor &reactor, HttpRequest req, http_success on_success,
                    http_failure on_failure) {
    url_parts u;
    if (!parse_url(req.url, u)) {
      on_failure("invalid URL: " + req.url);
      return;
    }

    auto stream = std::make_shared<BufferedStream>();
    std::string const request_text = encode_request(req, u);

    struct parser {
      bool headers_done = false;
      bool chunked = false;
      std::size_t content_remaining = 0;
      std::string header_block;
      std::string body;
      std::string leftover;
      HttpResponse response;
      bool done = false;
    };
    auto ps = std::make_shared<parser>();

    auto const finish_response = [reactor_sink = &reactor, stream, on_success,
                                  ps]() {
      (void)reactor_sink;
      if (ps->done)
        return;
      ps->done = true;
      on_success(ps->response);
    };

    stream->on_data = [stream, ps,
                       finish_response](std::span<const char> data) {
      std::string buf(data.begin(), data.end());
      buf.insert(buf.begin(), ps->leftover.begin(), ps->leftover.end());
      ps->leftover.clear();

      if (!ps->headers_done) {
        ps->header_block += buf;
        std::size_t const sep = ps->header_block.find("\r\n\r\n");
        if (sep == std::string::npos)
          return;
        std::string const head = ps->header_block.substr(0, sep);
        std::string const rest = ps->header_block.substr(sep + 4);
        ps->header_block.clear();
        ps->headers_done = true;

        std::istringstream hs(head);
        std::string line;
        std::size_t line_no = 0;
        while (std::getline(hs, line)) {
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          if (line_no == 0) {
            std::istringstream status(line);
            std::string proto;
            status >> proto >> ps->response.status;
            line_no = 1;
            continue;
          }
          std::size_t const colon = line.find(':');
          if (colon == std::string::npos)
            continue;
          std::string k = line.substr(0, colon);
          std::string v = line.substr(colon + 1);
          if (!v.empty() && v.front() == ' ')
            v.erase(v.begin());
          std::transform(k.begin(), k.end(), k.begin(), [](char c) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
          });
          ps->response.headers[std::move(k)] = std::move(v);
        }
        auto it = ps->response.headers.find("content-length");
        if (it != ps->response.headers.end())
          ps->content_remaining = sv::parse_or<std::size_t>(it->second, 0);
        else if (ps->response.headers.find("transfer-encoding") !=
                 ps->response.headers.end())
          ps->chunked = true;
        buf = rest;
      }

      if (!ps->chunked) {
        if (ps->content_remaining) {
          std::size_t const take = std::min(buf.size(), ps->content_remaining);
          ps->body.append(buf.data(), take);
          ps->content_remaining -= take;
        }
        if (ps->content_remaining == 0) {
          ps->response.body = std::move(ps->body);
          finish_response();
        }
      } else {
        // Minimal chunked decoder.
        while (!buf.empty()) {
          if (ps->content_remaining == 0) {
            std::size_t const crlf = buf.find("\r\n");
            if (crlf == std::string::npos) {
              ps->leftover = buf;
              return;
            }
            std::string const size_str = buf.substr(0, crlf);
            buf.erase(0, crlf + 2);
            unsigned long const sz =
                std::strtoul(size_str.c_str(), nullptr, 16);
            if (sz == 0) {
              ps->response.body = std::move(ps->body);
              finish_response();
              return;
            }
            ps->content_remaining = sz;
          }
          if (ps->content_remaining > buf.size()) {
            ps->body += buf;
            ps->content_remaining -= buf.size();
            buf.clear();
            continue;
          }
          ps->body.append(buf.data(), ps->content_remaining);
          buf.erase(0, ps->content_remaining);
          ps->content_remaining = 0;
          if (!buf.empty() && buf.front() == '\r')
            buf.erase(0, 1);
          if (!buf.empty() && buf.front() == '\n')
            buf.erase(0, 1);
        }
      }
    };

    stream->on_open = [stream, request_text]() { stream->send(request_text); };
    stream->on_tls_ready = stream->on_open;
    stream->on_close = [stream, on_success, on_failure, ps]() {
      if (ps->response.status != 0 && !ps->done) {
        ps->done = true;
        on_success(ps->response);
      } else if (!ps->done)
        on_failure("connection closed before a response was received");
    };

    if (!stream->connect(reactor, u.host, u.port, u.scheme == "https"))
      on_failure("connect failed: " + stream->error());
  }

} // namespace svc::net