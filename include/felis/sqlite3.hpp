#pragma once

#include "felis/common.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct sqlite3;
struct sqlite3_stmt;

// A minimal, Qt-free SQLite wrapper for the reconstructed store (design-1.md).
//
// This deliberately knows nothing about the schema: `connection` just opens a
// file and prepares statements, while `database` owns the tables. The legacy
// `lnb::sqlite_interface` could not be reused because its constructor runs the
// old schema and its migrations.
namespace felis::sqlite
{
// // //

class statement;


struct sqlite_binder : felis::iter_args<sqlite_binder> {
  // using felis::iter_args<sqlite_binder>::operator();

  sqlite_binder(const sqlite_binder&) = delete;
  sqlite_binder& operator=(const sqlite_binder&) = delete;

  explicit sqlite_binder(const statement& stmt)
    : stmt_(stmt) {
  }

  void operator()(std::string_view text);
  void operator()(const std::string& text) { (*this)(std::string_view(text)); }
  void operator()(int index, const char* text) {
     (*this)(std::string_view(text != nullptr ? text : ""));
  }
  void operator()(const std::optional<std::string>& text_opt);
  void operator()(int num);
  void operator()(uint32_t num);

  void operator()(void* ptr);

  const statement& stmt_;
  int count_ = 0;
};

// A prepared statement plus the connection lock it holds for its lifetime, so
// concurrent access to one connection is serialised. Move-only.
class statement {
  friend struct sqlite_binder;
public:
  statement() = delete;
  statement(const statement&) = delete;
  statement& operator=(const statement&) = delete;
  statement(statement&& other) noexcept;
  statement& operator=(statement&& other) noexcept;
  statement(sqlite3* db, sqlite3_stmt* stmt, std::unique_lock<std::mutex> lock);
  ~statement();

  // Binds arguments left to right to parameters 1..N. Overloads exist for the
  // value kinds the store uses; call sites cast explicitly for other integers.

  template<typename... Args>
  void bind(Args&&... args) const {
    sqlite_binder(*this).invoke(std::forward<Args>(args)...);
  }

  [[nodiscard]] bool step() const;
  void execute() const;
  [[nodiscard]] std::uint32_t execute_with_last_row_id() const;

  [[nodiscard]] int column_int(int index) const;
  [[nodiscard]] std::uint32_t column_uint32(int index) const;
  [[nodiscard]] std::string column_text(int index) const;
  [[nodiscard]] std::optional<std::string> column_text_opt(int index) const;

  // Number of rows changed by the most recent INSERT/UPDATE/DELETE.
  [[nodiscard]] int changes() const;

private:

  std::unique_lock<std::mutex> lock_;
  sqlite3_stmt* stmt_ = nullptr;
  sqlite3* db_ = nullptr;
};

class connection {
public:
  connection() = delete;
  explicit connection(std::string_view path);
  connection(const connection&) = delete;
  connection& operator=(const connection&) = delete;
  connection(connection&&) = delete;
  connection& operator=(connection&&) = delete;
  ~connection();

  [[nodiscard]] statement prepare(std::string_view sql) const;
  void exec(std::string_view sql) const;

private:
  sqlite3* db_ = nullptr;
  mutable std::mutex mutex_;
};

} // namespace lnb::store
