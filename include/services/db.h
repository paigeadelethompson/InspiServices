// InspiServices - SQLite data store.
//
// Everything (services config, nickserv/chanserv/botserv/operserv data, bridge
// routing) lives here. The only things *not* stored are link send/receive
// passwords and bot tokens which come from the environment / .env file.
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace svc {

  class db_error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  class db {
  public:
    using Value =
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;

    struct row {
      std::map<std::string, std::string> fields;

      [[nodiscard]] bool has(std::string_view key) const;
      [[nodiscard]] std::string
      as_string(std::string_view key) const; // "" if missing
      [[nodiscard]] std::int64_t as_int(std::string_view key,
                                        std::int64_t def = 0) const;
      [[nodiscard]] bool as_bool(std::string_view key, bool def = false) const;
    };

    using rows = std::vector<row>;

    explicit db(std::string path);
    ~db();

    db(const db &) = delete;
    db &operator=(const db &) = delete;

    // Executes a statement with bound parameters.
    void run(std::string_view sql, std::span<const Value> args = {});
    void run(std::string_view sql, std::initializer_list<Value> args) {
      run(sql, std::span(args.begin(), args.size()));
    }

    // Executes and returns all result rows.
    [[nodiscard]] rows query(std::string_view sql,
                             std::span<const Value> args = {});
    [[nodiscard]] rows query(std::string_view sql,
                             std::initializer_list<Value> args) {
      return query(sql, std::span(args.begin(), args.size()));
    }

    // Last inserted rowid, valid after a successful INSERT.
    [[nodiscard]] std::int64_t last_insert_rowid() const;

    // --- helpers ---------------------------------------------------------
    [[nodiscard]] bool table_exists(std::string_view name);
    void begin();
    void commit();
    void rollback();

    // Path this database object was opened with.
    [[nodiscard]] std::string const &path() const noexcept { return path_; }

  private:
    static int bind(sqlite3_stmt *stmt, std::span<const Value> const &vals,
                    std::string_view sql);

    std::string path_;
    sqlite3 *db_;
  };

  // shorthand so call sites read naturally
  using db_value = db::Value;

} // namespace svc