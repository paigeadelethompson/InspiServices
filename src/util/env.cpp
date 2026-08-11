#include "services/util/env.h"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <unordered_map>

namespace svc::env {

  namespace {
    std::unordered_map<std::string, std::string> g_file;
  }

  std::size_t load_env_file(std::string_view explicit_path) {
    if (!g_file.empty())
      return 0;

    std::string path;
    if (!explicit_path.empty())
      path = std::string(explicit_path);
    else if (char const *p = std::getenv("SERVICES_ENV_FILE"); p && *p)
      path = p;
    else
      path = ".env";

    std::size_t loaded = 0;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      line = sv::trim(line);
      if (line.empty() || line[0] == '#' || line[0] == ';')
        continue;
      auto const eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      std::string key = sv::trim(line.substr(0, eq));
      std::string value = sv::trim(line.substr(eq + 1));
      if (key.empty())
        continue;
      if (value.size() >= 2 &&
          ((value.front() == '"' && value.back() == '"') ||
           (value.front() == '\'' && value.back() == '\'')))
        value = value.substr(1, value.size() - 2);
      if (std::getenv(key.c_str()))
        continue; // Real environment wins.
      g_file[key] = value;
      ++loaded;
    }
    return loaded;
  }

  std::optional<std::string> get(std::string_view name) {
    if (char const *v = std::getenv(std::string(name).c_str()); v && *v)
      return std::string(v);
    auto it = g_file.find(std::string(name));
    if (it != g_file.end())
      return it->second;
    return std::nullopt;
  }

  std::optional<std::string> secret(std::string_view suffix) {
    std::string upper = std::string(suffix);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
      return static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    });

    if (auto v = get(std::string("SERVICES_") + upper); v)
      return v;
    return get(upper);
  }

  bool get_bool(std::string_view name, bool defval) {
    auto v = get(name);
    if (!v)
      return defval;
    std::string s = sv::trim(*v);
    if (sv::equals_ci(s, "1") || sv::equals_ci(s, "true") ||
        sv::equals_ci(s, "yes") || sv::equals_ci(s, "on"))
      return true;
    if (sv::equals_ci(s, "0") || sv::equals_ci(s, "false") ||
        sv::equals_ci(s, "no") || sv::equals_ci(s, "off"))
      return false;
    return defval;
  }

  int get_int(std::string_view name, int defval) {
    auto v = get(name);
    if (!v)
      return defval;
    return sv::parse_or<int>(*v, defval);
  }

} // namespace svc::env