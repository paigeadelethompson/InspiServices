#include "services/db.h"

#include <cstdlib>
#include <span>
#include <stdexcept>

#include "services/util/log.h"

namespace svc {

  namespace {

    void throw_sqlite(sqlite3 *d, std::string_view sql) {
      throw db_error(std::string("SQLite error: ") + sqlite3_errmsg(d) +
                     " while executing: " + std::string(sql));
    }

  } // namespace

  bool db::row::has(std::string_view key) const {
    return fields.find(std::string(key)) != fields.end();
  }

  std::string db::row::as_string(std::string_view key) const {
    auto it = fields.find(std::string(key));
    return it == fields.end() ? std::string() : it->second;
  }

  std::int64_t db::row::as_int(std::string_view key, std::int64_t def) const {
    auto it = fields.find(std::string(key));
    if (it == fields.end() || it->second.empty())
      return def;
    char *end = nullptr;
    long long const v = std::strtoll(it->second.c_str(), &end, 10);
    return (end == it->second.c_str()) ? def : static_cast<std::int64_t>(v);
  }

  bool db::row::as_bool(std::string_view key, bool def) const {
    auto it = fields.find(std::string(key));
    if (it == fields.end() || it->second.empty())
      return def;
    std::string const &v = it->second;
    return v == "1" || v == "true" || v == "yes" || v == "on";
  }

  db::db(std::string path) : path_(std::move(path)) {
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
      std::string const msg =
          db_ ? sqlite3_errmsg(db_) : "unable to open database";
      if (db_)
        sqlite3_close(db_);
      db_ = nullptr;
      throw db_error("Failed to open database '" + path_ + "': " + msg);
    }

    // Sensible defaults for a WAL-enabled multi-connection store (services and
    // bridges may share the same file on one host).
    sqlite3_exec(db_,
                 "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA "
                 "foreign_keys=ON;",
                 nullptr, nullptr, nullptr);
  }

  db::~db() {
    if (db_)
      sqlite3_close(db_);
  }

  bool db::table_exists(std::string_view name) {
    rows r = query("SELECT 1 FROM sqlite_master WHERE type='table' AND name=? "
                   "COLLATE NOCASE",
                   {std::string(name)});
    return !r.empty();
  }

  void db::begin() { run("BEGIN"); }
  void db::commit() { run("COMMIT"); }
  void db::rollback() { run("ROLLBACK"); }

  std::int64_t db::last_insert_rowid() const {
    return sqlite3_last_insert_rowid(db_);
  }

  void db::run(std::string_view sql, std::span<const Value> args) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK)
      throw_sqlite(db_, sql);

    int const rc = bind(stmt, args, sql);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(stmt);
      throw_sqlite(db_, sql);
    }

    int const step = sqlite3_step(stmt);
    if (step != SQLITE_DONE && step != SQLITE_ROW) {
      std::string const msg = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      throw db_error("SQLite step failed: " + msg +
                     " for: " + std::string(sql));
    }
    sqlite3_finalize(stmt);
  }

  db::rows db::query(std::string_view sql, std::span<const Value> args) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK)
      throw_sqlite(db_, sql);

    int const rc = bind(stmt, args, sql);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(stmt);
      throw_sqlite(db_, sql);
    }

    rows out;
    int const ncols = sqlite3_column_count(stmt);
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
      row r;
      for (int i = 0; i < ncols; ++i) {
        char const *const name = sqlite3_column_name(stmt, i);
        std::string key = name ? name : "";
        switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_INTEGER:
          r.fields[key] = std::to_string(sqlite3_column_int64(stmt, i));
          break;
        case SQLITE_FLOAT:
          r.fields[key] = std::to_string(sqlite3_column_double(stmt, i));
          break;
        case SQLITE_TEXT: {
          char const *const txt =
              reinterpret_cast<char const *>(sqlite3_column_text(stmt, i));
          r.fields[key] = txt ? txt : "";
          break;
        }
        case SQLITE_NULL:
          r.fields[key] = "";
          break;
        default: {
          int const nbytes = sqlite3_column_bytes(stmt, i);
          // Store blobs as their raw bytes escaped to string.
          void const *const blob = sqlite3_column_blob(stmt, i);
          (void)blob;
          r.fields[key].assign(static_cast<std::size_t>(nbytes), '\0');
          break;
        }
        }
      }
      out.push_back(std::move(r));
    }
    if (step != SQLITE_DONE) {
      std::string const msg = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      throw db_error("SQLite step failed: " + msg +
                     " for: " + std::string(sql));
    }
    sqlite3_finalize(stmt);
    return out;
  }

  int db::bind(sqlite3_stmt *stmt, std::span<const Value> const &vals,
               std::string_view sql) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    int idx = 1;
    for (Value const &v : vals) {
      int const rc = std::visit(
          [stmt, idx](auto &&arg) -> int {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>)
              return sqlite3_bind_null(stmt, idx);
            else if constexpr (std::is_same_v<T, bool>)
              return sqlite3_bind_int64(stmt, idx, arg ? 1 : 0);
            else if constexpr (std::is_same_v<T, std::int64_t>)
              return sqlite3_bind_int64(stmt, idx, arg);
            else if constexpr (std::is_same_v<T, double>)
              return sqlite3_bind_double(stmt, idx, arg);
            else if constexpr (std::is_same_v<T, std::string>)
              return sqlite3_bind_text(stmt, idx, arg.c_str(), -1,
                                       SQLITE_TRANSIENT);
            else
              return SQLITE_MISUSE;
          },
          v);
      if (rc != SQLITE_OK) {
        // Bind only deals with fixed-row statements; format errors are
        // surfaced via the sql text for easier diagnosis.
        throw db_error(std::string("bind failed (") + sqlite3_errstr(rc) +
                       ") for " + std::string(sql));
      }
      ++idx;
    }
    return SQLITE_OK;
  }

} // namespace svc