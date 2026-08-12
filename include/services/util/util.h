// InspiServices - general utilities.
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sv {

  namespace ranges {
    namespace views = std::views;
  }

  // ---------------------------------------------------------------------------
  // String helpers
  // ---------------------------------------------------------------------------
  [[nodiscard]] std::string trim(std::string_view s);
  [[nodiscard]] std::vector<std::string>
  split(std::string_view s, char sep,
        std::size_t maxParts = static_cast<std::size_t>(-1));
  [[nodiscard]] std::vector<std::string>
  splitws(std::string_view s); // whitespace separated
  [[nodiscard]] bool equals_ci(std::string_view a, std::string_view b);

  // RFC1459 case comparison (assumes remote uses rfc1459 casemapping).
  [[nodiscard]] bool irc_equals(std::string_view a, std::string_view b);
  [[nodiscard]] std::string irc_lower(std::string_view s);
  [[nodiscard]] char irc_lower(char c);

  // ---- Formatting helpers ----
  template <typename... Args>
  std::string fmt(const char *format, Args &&...args) {
    return std::vformat(format, std::make_format_args(args...));
  }

  template <typename T> [[nodiscard]] std::string to_string(T value) {
    return std::to_string(value);
  }

  [[nodiscard]] std::string join(const std::vector<std::string> &parts,
                                 std::string_view sep);

  // ---- Numeric parsing
  template <typename T>
  [[nodiscard]] bool try_parse(std::string_view s, T &out) {
    auto const r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
  }

  template <typename T> [[nodiscard]] T parse_or(std::string_view s, T def) {
    T r;
    return try_parse(s, r) ? r : def;
  }

  // ---- IRC lexical helpers -------------------------------------------------
  // Valid nickname: 1 to MaxNick letters, no spaces or :#,. within, and must
  // not begin with a digit or a '-'.
  [[nodiscard]] bool valid_nick(std::string_view nick);
  // Sanitise another protocol's display name into something nick-like.
  [[nodiscard]] std::string sanitize_nick(std::string_view nick,
                                          char fallback = '~');
  // Valid ident (username).
  [[nodiscard]] bool valid_ident(std::string_view ident);
  // Valid channel name.
  [[nodiscard]] bool valid_chan(std::string_view chan);

  // ---- Random / misc
  [[nodiscard]] std::string random_name(std::size_t len);
  [[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix);
  [[nodiscard]] bool ends_with(std::string_view s, std::string_view suffix);

} // namespace sv