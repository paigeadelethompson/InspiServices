#include "services/config.h"

#include <ctime>

#include "services/util/util.h"

namespace svc {

  config::config(db &database) : db_(database) {}

  std::optional<std::string> config::lookup(std::string_view key) const {
    auto rows = db_.query("SELECT value FROM config WHERE key=? COLLATE NOCASE",
                          {std::string(key)});
    if (rows.empty())
      return std::nullopt;
    return rows[0].as_string("value");
  }

  std::string config::required(std::string_view key) {
    auto v = lookup(key);
    if (!v)
      throw config_error(sv::fmt("required configuration key '{}' is missing "
                                 "(seed the database or set it via the DB)",
                                 key));
    return *v;
  }

  std::string config::as_string(std::string_view key, std::string_view def) {
    auto v = lookup(key);
    return v ? *v : std::string(def);
  }

  int config::as_int(std::string_view key, int def) {
    auto v = lookup(key);
    if (!v)
      return def;
    return sv::parse_or<int>(*v, def);
  }

  bool config::as_bool(std::string_view key, bool def) {
    auto v = lookup(key);
    if (!v)
      return def;
    std::string const s = sv::trim(*v);
    if (sv::equals_ci(s, "1") || sv::equals_ci(s, "true") ||
        sv::equals_ci(s, "yes") || sv::equals_ci(s, "on"))
      return true;
    if (sv::equals_ci(s, "0") || sv::equals_ci(s, "false") ||
        sv::equals_ci(s, "no") || sv::equals_ci(s, "off"))
      return false;
    return def;
  }

  std::vector<std::string> config::as_list(std::string_view key, char sep) {
    auto v = lookup(key);
    if (!v || v->empty())
      return {};
    return sv::split(*v, sep);
  }

  std::int64_t config::as_timestamp(std::string_view key, std::int64_t def) {
    auto v = lookup(key);
    if (!v)
      return def;
    return sv::parse_or<std::int64_t>(*v, def);
  }

  bool config::has(std::string_view key) const {
    return lookup(key).has_value();
  }

  void config::set(std::string_view key, std::string_view value) {
    db_.run("INSERT INTO config(key, value, updated_at) VALUES(?,?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value, "
            "updated_at=excluded.updated_at",
            {std::string(key), std::string(value),
             static_cast<std::int64_t>(std::time(nullptr))});
  }

} // namespace svc