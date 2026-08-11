// AnswerServices - minimal asynchronous HTTP/1.1 client.
//
// Used for the Discord REST API and optional signal-cli HTTP mode. Supports
// GET/POST/PATCH/DELETE, content-length and chunked responses.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace svc::net {

  class Reactor;

  struct HttpRequest {
    std::string method = "GET";
    std::string url; // e.g. https://discord.com/api/v10/...
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeout_ms = 20'000;
  };

  struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers; // lower-cased keys
    std::string body;
  };

  using http_success = std::function<void(HttpResponse const &)>;
  using http_failure = std::function<void(std::string const &)>;
  void http_request(Reactor &reactor, HttpRequest req, http_success on_success,
                    http_failure on_failure);

} // namespace svc::net