#include "sqlite_test_util.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

namespace zfleet::test {

namespace {

SQLite::Database OpenTestDatabase(const std::filesystem::path& database_path,
                                  const int flags) {
  SQLite::Database db(database_path.string(), flags);
  db.exec("PRAGMA busy_timeout=5000");
  return db;
}

}  // namespace

bool IsRecoverableBusy(const std::exception& ex) {
  const auto* sqlite_ex = dynamic_cast<const SQLite::Exception*>(&ex);
  if (sqlite_ex == nullptr) {
    return false;
  }
  return sqlite_ex->getErrorCode() == SQLITE_BUSY ||
         sqlite_ex->getErrorCode() == SQLITE_LOCKED;
}

int CountRows(const std::filesystem::path& database_path,
              std::string_view table_name) {
  for (int attempt = 0; attempt < kSqliteReadAttempts; ++attempt) {
    try {
      auto db = OpenTestDatabase(database_path, SQLite::OPEN_READONLY);
      SQLite::Statement query(db, "select count(*) from " +
                                      std::string(table_name));
      query.executeStep();
      return query.getColumn(0).getInt();
    } catch (const std::exception& ex) {
      if (!IsRecoverableBusy(ex) || attempt + 1 == kSqliteReadAttempts) {
        throw;
      }
      std::this_thread::sleep_for(kSqliteReadRetryDelay);
    }
  }
  throw std::runtime_error("failed to count rows");
}

int CountByQuery(const std::filesystem::path& database_path,
                 std::string_view query_text) {
  for (int attempt = 0; attempt < kSqliteReadAttempts; ++attempt) {
    try {
      auto db = OpenTestDatabase(database_path, SQLite::OPEN_READONLY);
      SQLite::Statement query(db, std::string(query_text));
      query.executeStep();
      return query.getColumn(0).getInt();
    } catch (const std::exception& ex) {
      if (!IsRecoverableBusy(ex) || attempt + 1 == kSqliteReadAttempts) {
        throw;
      }
      std::this_thread::sleep_for(kSqliteReadRetryDelay);
    }
  }
  throw std::runtime_error("failed to count query rows");
}

std::string ReadSingleTextColumn(const std::filesystem::path& database_path,
                                 std::string_view query_text,
                                 std::string_view parameter) {
  for (int attempt = 0; attempt < kSqliteReadAttempts; ++attempt) {
    try {
      auto db = OpenTestDatabase(database_path, SQLite::OPEN_READONLY);
      SQLite::Statement query(db, std::string(query_text));
      query.bind(1, std::string(parameter));
      query.executeStep();
      if (query.getColumn(0).isNull()) {
        return {};
      }
      return query.getColumn(0).getString();
    } catch (const std::exception& ex) {
      if (!IsRecoverableBusy(ex) || attempt + 1 == kSqliteReadAttempts) {
        throw;
      }
      std::this_thread::sleep_for(kSqliteReadRetryDelay);
    }
  }
  throw std::runtime_error("failed to read sqlite column");
}

}  // namespace zfleet::test
