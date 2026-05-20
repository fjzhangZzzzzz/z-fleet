#include "zfleet/package/temp_dir.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace zfleet::package {
namespace {

std::uint64_t CurrentProcessId() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

}  // namespace

ScopedTempDir::ScopedTempDir(std::string_view prefix)
    : base_dir_(std::filesystem::temp_directory_path() /
                std::filesystem::path(prefix)) {
  std::filesystem::create_directories(base_dir_);

  static std::atomic<std::uint64_t> counter{0};
  const auto process_id = CurrentProcessId();
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();

  for (int attempt = 0; attempt < 128; ++attempt) {
    const auto candidate =
        base_dir_ / (std::to_string(process_id) + "-" +
                     std::to_string(timestamp) + "-" +
                     std::to_string(counter.fetch_add(1)));
    std::error_code error;
    if (std::filesystem::create_directories(candidate, error) && !error) {
      path_ = candidate;
      return;
    }
  }

  throw std::runtime_error("failed to create temporary directory under: " +
                           base_dir_.string());
}

ScopedTempDir::~ScopedTempDir() {
  std::error_code error;
  if (!path_.empty()) {
    std::filesystem::remove_all(path_, error);
  }
  if (!base_dir_.empty()) {
    std::filesystem::remove(base_dir_, error);
  }
}

const std::filesystem::path& ScopedTempDir::path() const noexcept {
  return path_;
}

}  // namespace zfleet::package
