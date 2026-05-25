#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace zfleet::server {

struct StaticFileResponse {
  int status = 404;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;
};

class StaticFileService {
 public:
  explicit StaticFileService(std::filesystem::path root);

  void ValidateRequiredFiles() const;
  StaticFileResponse Read(std::string_view request_path) const;
  const std::filesystem::path& root() const noexcept;

 private:
  bool IsRegularFileWithoutSymlinks(
      const std::filesystem::path& relative_path) const;
  StaticFileResponse ReadFile(const std::filesystem::path& relative_path) const;

  std::filesystem::path root_;
};

}  // namespace zfleet::server
