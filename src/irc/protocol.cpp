#include "services/irc/protocol.h"

#include <cctype>
#include <cstring>
#include <ctime>

namespace svc::irc {
  namespace {

    bool is_alnum(char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9');
    }

    std::int64_t now_impl() {
      return static_cast<std::int64_t>(std::time(nullptr));
    }

    void split_first(std::string_view &line, std::string_view &head, char sep) {
      if (line.empty()) {
        head = {};
        return;
      }
      std::size_t const p = line.find(sep);
      if (p == std::string_view::npos) {
        head = line;
        line = {};
      } else {
        head = line.substr(0, p);
        line = line.substr(p + 1);
      }
    }

  } // namespace

  std::int64_t now() { return now_impl(); }

  bool is_mode_char(char c) {
    switch (c) {
    case 'o':
    case 'v':
    case 'q':
    case 'a':
    case 'h':
    case 'O':
    case 'u':
    case '+':
    case '-':
      return true;
    default:
      return false;
    }
  }
bool is_legal_sid(std::string_view s)
{
	// Per InspIRCd: exactly 3 chars, the first must be a digit, and the other
	// two must be A-Z or a digit (e.g. "8E0", "00A", "123").
	if (s.size() != 3)
		return false;
	if (!(s[0] >= '0' && s[0] <= '9'))
		return false;
	for (std::size_t i = 1; i < 3; ++i)
	{
		char const c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	}
	return true;
}

  bool is_legal_uid(std::string_view s) {
    if (s.size() < 6 || s.size() > 13)
      return false;
    for (char c : s)
      if (!is_alnum(c))
        return false;
    return true;
  }

  bool is_legal_nick(std::string_view s) {
    if (s.empty() || s.size() > 50)
      return false;
    // IRC nicks may not start with a digit, '#' or '-'.
    if (s[0] == '#' || s[0] == '-' || (s[0] >= '0' && s[0] <= '9'))
      return false;
    for (char c : s)
      if (!is_alnum(c) && c != '-' && c != '[' && c != ']' && c != '\\' &&
          c != '`' && c != '^' && c != '_' && c != '{' && c != '|' && c != '}')
        return false;
    return true;
  }

  message message::parse(std::string_view line) {
    message m;

    // Trailing CRLF.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.remove_suffix(1);

    if (line.empty())
      return m;

    // Tags: @a=b;c;d=1 ...
    if (line.front() == '@') {
      std::size_t const end = line.find(' ');
      if (end == std::string_view::npos)
        return m; // malformed
      std::string_view tagstr = line.substr(1, end - 1);
      line.remove_prefix(end + 1);
      while (!tagstr.empty()) {
        std::string_view item;
        split_first(tagstr, item, ';');
        std::size_t const eq = item.find('=');
        if (eq == std::string_view::npos)
          m.tags.emplace_back(std::string(item), std::string());
        else
          m.tags.emplace_back(std::string(item.substr(0, eq)),
                              std::string(item.substr(eq + 1)));
      }
    }

    // Prefix.
    if (!line.empty() && line.front() == ':') {
      line.remove_prefix(1);
      std::size_t const end = line.find(' ');
      if (end == std::string_view::npos) {
        m.prefix = std::string(line);
        return m;
      }
      m.prefix = std::string(line.substr(0, end));
      line.remove_prefix(end + 1);
    }

    // Command.
    {
      std::size_t const end = line.find(' ');
      if (end == std::string_view::npos) {
        m.command = std::string(line);
        for (char &c : m.command)
          if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - ('a' - 'A'));
        return m;
      }
      m.command = std::string(line.substr(0, end));
      for (char &c : m.command)
        if (c >= 'a' && c <= 'z')
          c = static_cast<char>(c - ('a' - 'A'));
      line.remove_prefix(end + 1);
    }

    // Parameters.
    while (!line.empty()) {
      if (line.front() == ':') {
        // Trailing: rest of line verbatim.
        line.remove_prefix(1);
        m.params.emplace_back(line);
        line = {};
        break;
      }
      std::size_t const end = line.find(' ');
      if (end == std::string_view::npos) {
        m.params.emplace_back(line);
        line = {};
      } else {
        m.params.emplace_back(line.substr(0, end));
        line.remove_prefix(end + 1);
      }
    }

    return m;
  }

  std::string message::to_wire() const {
    std::string out;
    if (prefix.empty())
      out = command;
    else {
      out.reserve(64 + params.size() * 8);
      if (!prefix.empty())
        out += ':' + prefix + ' ';
      out += command;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
      out.push_back(' ');
      // An IRC trailing parameter must be colon-prefixed when it contains
      // spaces (or would otherwise be split into multiple tokens), is empty,
      // or already begins with ':'. Non-trailing parameters are never
      // colon-prefixed.
      bool const trail = i + 1 == params.size();
      if (trail && (params[i].empty() || params[i][0] == ':' ||
                    params[i].find(' ') != std::string::npos ||
                    params[i].find('\t') != std::string::npos))
        out.push_back(':');
      out += params[i];
    }
    return out;
  }

  std::optional<std::string> message::tag(std::string_view key) const {
    for (auto const &[name, val] : tags)
      if (name == key)
        return val;
    return std::nullopt;
  }

  std::string message::param_or(std::size_t idx, std::string_view def) const {
    if (idx >= params.size())
      return std::string(def);
    return params[idx];
  }

  std::string build_line(std::string_view prefix, std::string_view command,
                         std::span<const std::string> params) {
    std::string out;
    if (!prefix.empty()) {
      out.push_back(':');
      out += prefix;
      out.push_back(' ');
    }
    out += command;
    for (std::size_t i = 0; i < params.size(); ++i) {
      out.push_back(' ');
      if (i + 1 == params.size())
        out.push_back(':');
      out += params[i];
    }
    return out;
  }

  std::string prefix_last(std::string_view text) {
    std::string out;
    if (text.empty() || text[0] == ':')
      out.push_back(':');
    out += text;
    return out;
  }

  std::vector<std::string> parse_member_list(std::string_view joined) {
    std::vector<std::string> list;
    // The trailing FJOIN parameter is a SPACE-separated list of members. Each
    // entry is "[prefixmodes,]uuid[:membid]" (the comma and mode letters are
    // omitted when the user has no prefix modes, e.g. InspIRCd 3.x+).
    // Prefix mode letters are kept attached so callers can parse them.
    while (!joined.empty()) {
      joined.remove_prefix(joined.find_first_not_of(' '));
      if (joined.empty())
        break;
      std::string_view one;
      std::size_t const sp = joined.find(' ');
      if (sp == std::string_view::npos) {
        one = joined;
        joined = {};
      } else {
        one = joined.substr(0, sp);
        joined = joined.substr(sp + 1);
      }
      if (one.empty() || one[0] == ':')
        continue;
      list.push_back(std::string(one));
    }
    return list;
  }

} // namespace svc::irc