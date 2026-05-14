#pragma once

#include <filesystem>

namespace zfleet::server {

class ServerDatabase {
 public:
  explicit ServerDatabase(std::filesystem::path database_path);

  void Initialize();
  int schema_version() const;
  const std::filesystem::path& database_path() const noexcept;

 private:
  std::filesystem::path database_path_;
};

} // namespace zfleet::server
