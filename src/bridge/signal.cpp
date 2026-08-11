// AnswerServices - native Signal bridge (JSON-RPC over TCP to signal-cli).
#include "services/bridge/signal.h"

#include <chrono>
#include <sstream>
#include <utility>

#include <json/json.h>

#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::bridge {

  namespace {
    constexpr net::Reactor::Handle no_timer = net::Reactor::bad_handle;

    std::string field_str(Json::Value const &v, std::string_view key) {
      return v.isMember(std::string(key)) ? v[std::string(key)].asString()
                                          : std::string();
    }

  } // namespace

  signal_bridge::signal_bridge(net::Reactor &reactor, std::string host,
                               std::string port, std::string account)
      : reactor_(reactor), host_(std::move(host)), port_(std::move(port)),
        account_(std::move(account)) {}

  signal_bridge::~signal_bridge() {
    stream_.close();
    if (resume_timer_ != no_timer)
      reactor_.remove_timer(resume_timer_);
  }

  void signal_bridge::start() {
    backoff_ = 1;
    connect();
  }

  void signal_bridge::connect() {
    if (resume_timer_ != no_timer) {
      reactor_.remove_timer(resume_timer_);
      resume_timer_ = no_timer;
    }
    connected_ = false;

    stream_.on_open = [this]() {
      connected_ = true;
      backoff_ = 1;
      log::info("signal", "connected to {}:{}", host_, port_);
      if (on_state)
        on_state(true);
    };
    stream_.on_data = [this](std::span<const char> d) {
      rx_.append(d.data(), d.size());
      pump_rx();
    };
    stream_.on_close = [this]() {
      connected_ = false;
      if (on_state)
        on_state(false);
      schedule_resume();
    };

    if (!stream_.connect(reactor_, host_, port_, false)) {
      log::warn("signal", "connect failed to {}:{}", host_, port_);
      schedule_resume();
    }
  }

  void signal_bridge::pump_rx() {
    while (connected_) {
      auto pos = rx_.find('\n');
      if (pos == std::string::npos)
        break;
      std::string_view line(rx_.data(), pos);
      handle_line(line);
      rx_.erase(0, pos + 1);
    }
  }

  void signal_bridge::handle_line(std::string_view line) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream ss(std::string(line.data(), line.size()));
    std::string errs;
    if (!Json::parseFromStream(builder, ss, &root, &errs)) {
      log::warn("signal", "JSON parse error: {}", errs);
      return;
    }

    // JSON-RPC response (has "id") — ignore.
    if (root.isMember("id"))
      return;

    // Notification: "method" == "data" carries a message.
    std::string const method = field_str(root, "method");
    if (method == "data") {
      Json::Value const &params = root["params"];
      std::string const from = field_str(params, "sender");
      std::string const body = field_str(params, "body");
      if (!from.empty() && !body.empty()) {
        if (on_message)
          on_message(from, from, body);
      }
    }
  }

  void signal_bridge::send_message(std::string_view recipient,
                                   std::string_view text) {
    Json::Value req(Json::objectValue);
    req["jsonrpc"] = "2.0";
    req["id"] = static_cast<Json::Int64>(next_id_++);

    Json::Value args(Json::objectValue);
    args["recipient"] = std::string(recipient);
    args["text"] = std::string(text);
    args["account"] = account_;

    req["method"] = "send";
    req["params"] = args;

    Json::StreamWriterBuilder wb;
    std::string out = Json::writeString(wb, req);
    out.push_back('\n');
    stream_.send(out);
  }

  void signal_bridge::send_group_message(std::string_view group_id,
                                         std::string_view text) {
    Json::Value req(Json::objectValue);
    req["jsonrpc"] = "2.0";
    req["id"] = static_cast<Json::Int64>(next_id_++);

    Json::Value args(Json::objectValue);
    args["groupId"] = std::string(group_id);
    args["text"] = std::string(text);
    args["account"] = account_;

    req["method"] = "send";
    req["params"] = args;

    Json::StreamWriterBuilder wb;
    std::string out = Json::writeString(wb, req);
    out.push_back('\n');
    stream_.send(out);
  }

  void signal_bridge::schedule_resume() {
    if (resume_timer_ != no_timer)
      return;
    unsigned const delay = backoff_;
    backoff_ = std::min<unsigned>(backoff_ * 2, 60);
    resume_timer_ = reactor_.add_timeout(std::chrono::seconds(delay), [this]() {
      resume_timer_ = no_timer;
      connect();
    });
    log::info("signal", "reconnecting in {}s", delay);
  }

  // Build the JSON-RPC "send" request for external callers.
  std::string signal_send_request(std::string_view account,
                                  std::string_view recipient,
                                  std::string_view text, bool is_group,
                                  std::uint64_t id) {
    Json::Value req(Json::objectValue);
    req["jsonrpc"] = "2.0";
    req["id"] = static_cast<Json::Int64>(id);

    Json::Value args(Json::objectValue);
    if (is_group)
      args["groupId"] = std::string(recipient);
    else
      args["recipient"] = std::string(recipient);
    args["text"] = std::string(text);
    args["account"] = std::string(account);

    req["method"] = "send";
    req["params"] = args;

    Json::StreamWriterBuilder wb;
    return Json::writeString(wb, req) + "\n";
  }

} // namespace svc::bridge