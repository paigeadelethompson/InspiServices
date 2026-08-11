// AnswerServices - native Discord bridge implementation.
#include "services/bridge/discord.h"

#include <chrono>
#include <sstream>
#include <utility>

#include <json/json.h>

#include "services/util/env.h"
#include "services/util/log.h"
#include "services/util/util.h"

namespace svc::bridge {

  namespace {
    // Discord v10 intents: GUILDS | GUILD_MESSAGES | DIRECT_MESSAGES |
    // MESSAGE_CONTENT.
    constexpr std::int64_t k_intents =
        (1 << 0) | (1 << 9) | (1 << 12) | (1 << 15);
    constexpr net::Reactor::Handle no_timer = net::Reactor::bad_handle;

    std::string const &fallback_url() {
      static std::string const url =
          "wss://gateway.discord.gg/?v=10&encoding=json";
      return url;
    }

    std::int64_t field_int(Json::Value const &v, std::string_view key) {
      return v.isMember(std::string(key)) ? v[std::string(key)].asInt64() : 0;
    }

    std::string field_str(Json::Value const &v, std::string_view key) {
      return v.isMember(std::string(key)) ? v[std::string(key)].asString()
                                          : std::string();
    }
  } // namespace

  discord_bridge::discord_bridge(net::Reactor &reactor, std::string host,
                                 std::string token)
      : host_(std::move(host)), token_(std::move(token)), reactor_(reactor) {
    ws_.on_close = [this]() {
      ready_ = false;
      identified_ = false;
      if (heartbeat_timer_ != no_timer) {
        reactor_.remove_timer(heartbeat_timer_);
        heartbeat_timer_ = no_timer;
      }
      if (on_state)
        on_state(false);
      schedule_reconnect();
    };
    ws_.on_error = [this](std::string const &msg) {
      svc::log::warn("discord", "connection error: {}", msg);
    };
  }

  discord_bridge::~discord_bridge() {
    ws_.close();
    if (heartbeat_timer_ != no_timer)
      reactor_.remove_timer(heartbeat_timer_);
    if (reconnect_timer_ != no_timer)
      reactor_.remove_timer(reconnect_timer_);
  }

  void discord_bridge::start() {
    backoff_ = 1;
    connect_gateway(fallback_url());
  }

  void discord_bridge::connect_gateway(std::string const &url) {
    if (reconnect_timer_ != no_timer) {
      reactor_.remove_timer(reconnect_timer_);
      reconnect_timer_ = no_timer;
    }
    identified_ = false;
    ready_ = false;

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Authorization", "Bot " + token_);

    ws_.on_open = [this]() { send_identify(); };
    ws_.on_text = [this](std::span<const char> d) { on_ws_text(d); };

    if (ws_.connect(reactor_, url, headers))
      svc::log::info("discord", "connecting to gateway");
    else {
      svc::log::warn("discord", "gateway connection not started");
      schedule_reconnect();
    }
  }

  void discord_bridge::on_ws_text(std::span<const char> data) {
    std::string text(data.begin(), data.end());
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream ss(text);
    std::string errs;
    if (!Json::parseFromStream(builder, ss, &root, &errs)) {
      svc::log::warn("discord", "gateway: parse error: {}", errs);
      return;
    }

    std::int64_t const op = field_int(root, "op");
    if (root.isMember("s"))
      last_seq_ = static_cast<int>(root["s"].asInt64());

    switch (op) {
    case 10: // Hello
    {
      Json::Value d = root["d"];
      std::int64_t const interval = field_int(d, "heartbeat_interval");
      if (interval > 0) {
        heartbeat_timer_ = reactor_.add_timer(
            std::chrono::milliseconds(interval), [this]() { heartbeat(); },
            true);
      }
      heartbeat();
      send_identify();
      break;
    }
    case 0: // Dispatch
    {
      std::string const t = field_str(root, "t");
      Json::Value const &d = root["d"];
      if (t == "READY") {
        ready_ = true;
        backoff_ = 1;
        session_id_ = field_str(d, "session_id");
        svc::log::info("discord", "gateway READY");
        if (on_state)
          on_state(true);
      } else if (t == "MESSAGE_CREATE") {
        std::string const channel = field_str(d, "channel_id");
        if (channel.empty())
          break;
        Json::Value author = d["author"];
        std::string const who = field_str(author, "username");
        std::string const content = field_str(d, "content");
        std::string const msgtype = field_str(d, "type");
        if (content.empty() || msgtype != "0")
          break;

        if (on_message)
          on_message(channel, who, content);
      }
      break;
    }
    case 11: // Heartbeat ACK
      break;
    default:
      break;
    }
  }

  void discord_bridge::send_identify() {
    if (identified_)
      return;
    identified_ = true;
    last_seq_ = 0;

    Json::Value properties(Json::objectValue);
    properties["$os"] = "linux";
    properties["$browser"] = "AnswerServices";
    properties["$device"] = "AnswerServices";

    Json::Value d(Json::objectValue);
    d["token"] = token_;
    d["intents"] = k_intents;
    d["properties"] = properties;
    d["compress"] = false;

    Json::Value payload(Json::objectValue);
    payload["op"] = 2;
    payload["d"] = d;

    Json::StreamWriterBuilder wb;
    ws_.send_text(Json::writeString(wb, payload));
  }

  void discord_bridge::heartbeat() {
    Json::Value payload(Json::objectValue);
    payload["op"] = 1;
    payload["d"] = static_cast<Json::Int64>(last_seq_);
    Json::StreamWriterBuilder wb;
    ws_.send_text(Json::writeString(wb, payload));
  }

  void discord_bridge::schedule_reconnect() {
    if (reconnect_timer_ != no_timer)
      return;
    unsigned const delay = backoff_;
    backoff_ = std::min<unsigned>(backoff_ * 2, 60);
    reconnect_timer_ =
        reactor_.add_timeout(std::chrono::seconds(delay), [this]() {
          reconnect_timer_ = no_timer;
          connect_gateway(fallback_url());
        });
    svc::log::info("discord", "reconnecting in {}s", delay);
  }

  void discord_bridge::send_message(std::string_view channel_id,
                                    std::string_view text) {
    net::HttpRequest req;
    req.method = "POST";
    req.url = "https://discord.com/api/v10/channels/" +
              std::string(channel_id) + "/messages";
    req.headers.emplace_back("Authorization", "Bot " + token_);
    req.headers.emplace_back("Content-Type", "application/json");
    req.body = discord_body(text);

    net::http_request(
        reactor_, std::move(req),
        [](net::HttpResponse const &resp) {
          if (resp.status >= 300)
            svc::log::warn("discord", "send: HTTP {}", resp.status);
        },
        [](std::string const &err) {
          svc::log::warn("discord", "send error: {}", err);
        });
  }

  std::string discord_body(std::string_view content) {
    Json::Value p(Json::objectValue);
    p["content"] = std::string(content);
    Json::StreamWriterBuilder wb;
    return Json::writeString(wb, p);
  }

} // namespace svc::bridge