#include "hstream/src/libs/recording/Database.h"
#include "hstream/src/libs/recording/Schema.h"
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <sys/random.h>
#include <cerrno>

namespace hm::recording {
Statement::Statement(sqlite3* db, const std::string& sql) {
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}
Statement::~Statement() { sqlite3_finalize(statement_); }
void Statement::Check(int code) const {
  if (code != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement_)));
}
void Statement::Bind(int c, const std::string& v) { Check(sqlite3_bind_text(statement_, c, v.data(), v.size(), SQLITE_TRANSIENT)); }
void Statement::Blob(int c, const std::string& v) { Check(sqlite3_bind_blob(statement_, c, v.data(), v.size(), SQLITE_TRANSIENT)); }
void Statement::Bind(int c, double v) { Check(sqlite3_bind_double(statement_, c, v)); }
void Statement::Bind(int c, uint64_t v) {
  if (v > uint64_t(std::numeric_limits<int64_t>::max())) throw std::runtime_error("Telemetry integer exceeds SQLite range");
  Check(sqlite3_bind_int64(statement_, c, v));
}
void Statement::Bind(int c, int v) { Check(sqlite3_bind_int(statement_, c, v)); }
void Statement::Null(int c) { Check(sqlite3_bind_null(statement_, c)); }
bool Statement::Next() {
  const int code = sqlite3_step(statement_);
  if (code == SQLITE_ROW) return true;
  if (code == SQLITE_DONE) return false;
  throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement_)));
}
void Statement::Execute() { if (Next()) throw std::runtime_error("Unexpected SQL row"); Reset(); }
void Statement::Reset() { Check(sqlite3_reset(statement_)); Check(sqlite3_clear_bindings(statement_)); }
std::string Statement::Text(int c) const {
  const auto* p = sqlite3_column_text(statement_, c);
  return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(statement_, c)) : std::string();
}
std::string Statement::Bytes(int c) const {
  const auto* p = sqlite3_column_blob(statement_, c);
  return p ? std::string(static_cast<const char*>(p), sqlite3_column_bytes(statement_, c)) : std::string();
}
int64_t Statement::Int(int c) const { return sqlite3_column_int64(statement_, c); }
double Statement::Real(int c) const { return sqlite3_column_double(statement_, c); }
bool Statement::IsNull(int c) const { return sqlite3_column_type(statement_, c) == SQLITE_NULL; }
Database::Database(const std::string& path, bool writable) {
  const int code = sqlite3_open_v2(path.c_str(), &db_, writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, nullptr);
  if (code != SQLITE_OK) {
    const std::string error = db_ ? sqlite3_errmsg(db_) : "Cannot allocate SQLite connection";
    sqlite3_close(db_); db_ = nullptr; throw std::runtime_error(path + ": " + error);
  }
  sqlite3_busy_timeout(db_, 5000);
  Exec("PRAGMA foreign_keys=ON");
}
Database::~Database() { sqlite3_close_v2(db_); }
void Database::Exec(const std::string& sql) {
  char* message = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
    std::string error = message ? message : sqlite3_errmsg(db_);
    sqlite3_free(message); throw std::runtime_error(error);
  }
}
void Database::Validate() {
  Statement app(db_, "PRAGMA application_id");
  Statement version(db_, "PRAGMA user_version");
  if (!app.Next() || app.Int(0) != 1213027156 || !version.Next() || version.Int(0) != 1)
    throw std::runtime_error("Unsupported hockey telemetry database schema");
}
std::string NewGuid() {
  std::array<unsigned char,16> bytes{};
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = getrandom(bytes.data()+offset, bytes.size()-offset, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("Cannot generate recording GUID");
    offset += n;
  }
  bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;
  std::ostringstream out;
  for (size_t i=0;i<bytes.size();++i) {
    if (i==4 || i==6 || i==8 || i==10) out << '-';
    out << std::hex << std::setw(2) << std::setfill('0') << int(bytes[i]);
  }
  return out.str();
}
const char* Schema() { return kRecordingSchema; }
} // namespace hm::recording
