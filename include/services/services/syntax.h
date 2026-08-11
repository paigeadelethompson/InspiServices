// AnswerServices - command text utilities shared by all services.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svc::core {

  // Splits "word1 word2 ..." into words. Responsible for mIRC-style колон
  // prefixes.
  std::vector<std::string> split_words(std::string_view text);

  // Pre: text like "COMMAND arg1 arg2". Returns the upper-cased command and
  // args.
  void parse_command(std::string_view text, std::string &command,
                     std::vector<std::string> &args);

  // Parses an integer in base-10.
  std::optional<std::int64_t> parse_int(std::string_view s);

  // Parses a duration string like "2h", "30m", "1d6h30m" into seconds.
  std::optional<std::int64_t> parse_duration(std::string_view s);

  // Formats a seconds count into a friendly "1d 02:03:04".
  std::string format_duration(std::int64_t secs);

} // namespace svc::core