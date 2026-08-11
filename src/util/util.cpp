#include "services/util/util.h"

#include <cctype>
#include <random>

namespace sv {

  std::string trim(std::string_view s) {
    std::size_t const b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos)
      return {};
    std::size_t const e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
  }

  std::vector<std::string> split(std::string_view s, char sep,
                                 std::size_t maxParts) {
    std::vector<std::string> out;
    if (s.empty()) {
      out.emplace_back();
      return out;
    }
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size() && out.size() + 1 < maxParts; ++i) {
      if (s[i] == sep) {
        out.emplace_back(s.substr(start, i - start));
        start = i + 1;
      }
    }
    out.emplace_back(s.substr(start));
    return out;
  }

  std::vector<std::string> splitws(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
      std::size_t const start = i;
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
      if (i > start)
        out.emplace_back(s.substr(start, i - start));
    }
    return out;
  }

  char irc_lower(char c) {
    if (c >= 'A' && c <= 'Z')
      return static_cast<char>(c + ('a' - 'A'));
    // RFC1459 special-cases []\~ and ^ (but not |).
    if (c == '[')
      return '{';
    if (c == ']')
      return '}';
    if (c == '\\')
      return '|';
    if (c == '~')
      return '^';
    return c;
  }

  std::string irc_lower(std::string_view s) {
    std::string out(s);
    for (char &c : out)
      c = irc_lower(c);
    return out;
  }

  bool irc_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
      return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (irc_lower(a[i]) != irc_lower(b[i]))
        return false;
    return true;
  }

  bool equals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
      return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      unsigned char ca = static_cast<unsigned char>(a[i]);
      unsigned char cb = static_cast<unsigned char>(b[i]);
      if (std::tolower(ca) != std::tolower(cb))
        return false;
    }
    return true;
  }

  std::string join(const std::vector<std::string> &parts,
                   std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i)
        out.append(sep);
      out.append(parts[i]);
    }
    return out;
  }

  bool valid_nick(std::string_view nick) {
    // Prefix rules: cannot start with a digit, '-', or be a network numeric.
    if (nick.empty() || nick.size() > 32)
      return false;
    char const c0 = nick[0];
    if (std::isdigit(static_cast<unsigned char>(c0)) || c0 == '-' || c0 == '$')
      return false;
    for (char const c : nick) {
      if (c == ' ' || c == ':' || c == '#' || c == ',' || c == '*' ||
          c == '?' || c == '.')
        return false;
    }
    return true;
  }

  std::string sanitize_nick(std::string_view nick, char fallback) {
    std::string out;
    for (char const c : nick) {
      unsigned char const uc = static_cast<unsigned char>(c);
      if (uc < 0x20 || c == ' ' || c == ':' || c == '#' || c == ',' ||
          c == '.' || c == '!' || c == '@' || c == '=' || c == '*' ||
          c == '?' || c == ']' || c == '[' || c == '{' || c == '}' ||
          c == '\\' || c == '|' || c == '^' || c == '~') {
        if (out.empty())
          out.push_back(fallback);
        continue;
      }
      out.push_back(c);
      if (out.size() >= 32)
        break;
    }
    if (out.empty())
      return std::string(1, fallback);
    if (std::isdigit(static_cast<unsigned char>(out[0])) || out[0] == '-')
      out.insert(out.begin(), fallback);
    return out;
  }

  bool valid_ident(std::string_view ident) {
    if (ident.empty() || ident.size() > 12)
      return false;
    for (char const c : ident) {
      if (c == ' ' || c == '@' || c == '!' || c == ':' || c == ']' ||
          c == '[' || c == '~')
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(ident[0])) ? false : true;
  }

  bool valid_chan(std::string_view chan) {
    if (chan.empty() || chan.size() < 2 || chan.size() > 64)
      return false;
    if (chan[0] != '#' && chan[0] != '&')
      return false;
    if (chan[1] == '#' || chan[1] == ' ' || chan.size() > 200)
      return false;
    return true;
  }

  bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
  }

  bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
  }

  std::string random_name(std::size_t len) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
      out.push_back(alphabet[rng() % alphabet.size()]);
    return out;
  }

} // namespace sv