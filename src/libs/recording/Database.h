#pragma once

#include <sqlite3.h>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace hm::recording {
class Statement {
 public:
  Statement(sqlite3* db, const std::string& sql);
  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  void Bind(int column, const std::string& value);
  void Bind(int column, const char* value) { Bind(column, std::string(value)); }
  void Bind(int column, double value);
  void Bind(int column, uint64_t value);
  void Bind(int column, uint32_t value) { Bind(column, uint64_t(value)); }
  void Bind(int column, int value);
  template <class T> void Bind(int column, const std::optional<T>& value) {
    if (value) Bind(column, *value); else Null(column);
  }
  void Blob(int column, const std::string& value);
  void Null(int column);
  bool Next();
  void Execute();
  void Reset();
  std::string Text(int column) const;
  std::string Bytes(int column) const;
  int64_t Int(int column) const;
  double Real(int column) const;
  bool IsNull(int column) const;
 private:
  void Check(int code) const;
  sqlite3_stmt* statement_{nullptr};
};
class Database {
 public:
  explicit Database(const std::string& path, bool writable = false);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  sqlite3* get() const { return db_; }
  void Exec(const std::string& sql);
  void Validate();
 private:
  sqlite3* db_{nullptr};
};
std::string NewGuid();
const char* Schema();
} // namespace hm::recording
