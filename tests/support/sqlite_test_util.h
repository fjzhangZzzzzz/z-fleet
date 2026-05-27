#pragma once

#include <cstdint>
#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace zfleet::test {

constexpr int kSqliteReadAttempts = 50;
constexpr auto kSqliteReadRetryDelay = std::chrono::milliseconds(20);

bool IsRecoverableBusy(const std::exception& ex);
int CountRows(const std::filesystem::path& database_path,
              std::string_view table_name);
int CountByQuery(const std::filesystem::path& database_path,
                 std::string_view query_text);
std::string ReadSingleTextColumn(const std::filesystem::path& database_path,
                                 std::string_view query_text,
                                 std::string_view parameter);

}  // namespace zfleet::test
