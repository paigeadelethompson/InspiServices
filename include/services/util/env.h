// InspiServices - environment / .env handling and secrets.
//
// Secrets (link send/receive passwords, bot tokens) are never stored in the
// database. They are read from (in priority order):
//   1. a shared/set environment variable (SERVICES_SENDPASS_SERVICESNAME etc.)
//   2. a simple KEY=VALUE .env file next to the executable or given by
//      SERVICES_ENV_FILE.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "services/util/util.h"

namespace svc::env {

  // Loads the .env file (once). Lines are KEY=VALUE (no quoting). Returns the
  // number of newly set variables that weren't already in the environment.
  std::size_t load_env_file(std::string_view explicit_path = {});

  // Returns the value of an environment variable, if set (either inherited or
  // from the .env file).
  std::optional<std::string> get(std::string_view name);

  // Fetches a secret. Looks first for SERVICES_<SUFFIX> then <SUFFIX>.
  std::optional<std::string> secret(std::string_view suffix);

  [[nodiscard]] bool get_bool(std::string_view name, bool defval = false);
  [[nodiscard]] int get_int(std::string_view name, int defval = 0);

} // namespace svc::env