// InspiServices - configuration access.
//
// All runtime configuration lives in the `config` table (key -> value). Values
// may be overwritten at runtime by opers (e.g. +aservices set). Secrets
// (link send/recv passwords, bridge tokens) are *never* stored here; they are
// resolved from the environment/.env file through svc::env::secret().
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "services/db.h"

namespace svc {

  class config_error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  class config {
  public:
    explicit config(db &db);

    // --- typed getters (all fall back to a default) -----------------------
    std::string as_string(std::string_view key, std::string_view def = {});
    int as_int(std::string_view key, int def = 0);
    bool as_bool(std::string_view key, bool def = false);
    std::vector<std::string> as_list(std::string_view key, char sep = ',');
    std::int64_t as_timestamp(std::string_view key, std::int64_t def = 0);

    // Required form: raises an exception if the key is not present.
    std::string required(std::string_view key);

    // Returns true if the key exists in the database.
    bool has(std::string_view key) const;

    // Stores a value back into the database (also usable by runtime SET).
    void set(std::string_view key, std::string_view value);

    // The database used by this config.
    [[nodiscard]] db &database() noexcept { return db_; }

  private:
    std::optional<std::string> lookup(std::string_view key) const;

    db &db_;
  };

} // namespace svc