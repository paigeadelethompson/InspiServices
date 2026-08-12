// InspiServices - command text utilities shared by all services.
#include "services/services/syntax.h"

#include <cctype>
#include <charconv>

#include "services/util/util.h"

namespace svc::core {

  std::vector<std::string> split_words(std::string_view text) {
    return sv::splitws(text);
  }

  void parse_command(std::string_view text, std::string &command,
                     std::vector<std::string> &args) {
    std::vector<std::string> words = split_words(text);
    if (words.empty()) {
      command.clear();
      return;
    }
    command = std::move(words[0]);
    for (char &c : command)
      if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - ('a' - 'A'));
    args.assign(words.begin() + 1, words.end());
  }

  std::optional<std::int64_t> parse_int(std::string_view s) {
    std::int64_t v = 0;
    auto const r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size())
      return std::nullopt;
    return v;
  }

  std::optional<std::int64_t> parse_duration(std::string_view s) {
    std::int64_t total = 0;
    std::int64_t cur = 0;
    bool any = false;
    for (char c : s) {
      if (c >= '0' && c <= '9') {
        cur = cur * 10 + static_cast<std::int64_t>(c - '0');
        if (cur > 100000000)
          return std::nullopt;
      } else {
        std::int64_t mult;
        switch (c) {
        case 's':
          mult = 1;
          break;
        case 'm':
          mult = 60;
          break;
        case 'h':
          mult = 3600;
          break;
        case 'd':
          mult = 86400;
          break;
        case 'w':
          mult = 7 * 86400;
          break;
        default:
          return std::nullopt;
        }
        if (cur == 0 && total == 0)
          return std::nullopt;
        total += cur * mult;
        cur = 0;
        any = true;
      }
    }
    if (cur != 0) {
      // trailing bare number means seconds
      total += cur;
      any = true;
    }
    return any ? std::optional<std::int64_t>(total) : std::nullopt;
  }

  std::string format_duration(std::int64_t secs) {
    if (secs < 0)
      secs = 0;
    std::int64_t const d = secs / 86400;
    std::int64_t const h = (secs % 86400) / 3600;
    std::int64_t const m = (secs % 3600) / 60;
    std::int64_t const s = secs % 60;
    if (d > 0)
      return sv::fmt("{}d {:02}:{:02}:{:02}", d, h, m, s);
    return sv::fmt("{:02}:{:02}:{:02}", h, m, s);
  }

} // namespace svc::core