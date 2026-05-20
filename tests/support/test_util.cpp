#include "test_util.h"

#include <atomic>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace zfleet::test {

namespace {

std::filesystem::path MakeTestDirPath(std::string_view suite_name) {
  static std::atomic<std::uint64_t> g_sequence{0};
  const auto pid = CurrentProcessId();
  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed);
  return std::filesystem::temp_directory_path() / std::filesystem::path(suite_name) /
         (std::to_string(pid) + "-" + std::to_string(sequence));
}

void RemoveEmptyParents(const std::filesystem::path& leaf_path) {
  std::error_code error;
  const auto temp_root = std::filesystem::temp_directory_path(error);
  if (error) {
    return;
  }

  auto current = leaf_path.parent_path();
  while (!current.empty() && current != temp_root) {
    std::error_code remove_error;
    if (!std::filesystem::remove(current, remove_error)) {
      break;
    }
    current = current.parent_path();
  }
}

}  // namespace

std::uint64_t CurrentProcessId() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

std::filesystem::path operator/(const ScopedTestDir& directory,
                                const std::filesystem::path& child) {
  return directory.path() / child;
}

ScopedTestDir::ScopedTestDir(std::string_view suite_name)
    : path_(MakeTestDirPath(suite_name)) {
  std::error_code error;
  std::filesystem::create_directories(path_, error);
  if (error) {
    throw std::runtime_error("failed to create test directory: " +
                             path_.string());
  }
}

ScopedTestDir::~ScopedTestDir() {
  if (path_.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::remove_all(path_, error);
  RemoveEmptyParents(path_);
}

const std::filesystem::path& ScopedTestDir::path() const noexcept {
  return path_;
}

ScopedTestDir::operator const std::filesystem::path&() const noexcept {
  return path_;
}

void WriteTextFile(const std::filesystem::path& path,
                   std::string_view content) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    throw std::runtime_error("failed to create parent directories for: " +
                             path.string());
  }

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("failed to open file for writing: " +
                             path.string());
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open file for reading: " +
                             path.string());
  }

  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

void SetExecutable(const std::filesystem::path& path) {
#ifdef _WIN32
  (void)path;
#else
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  (void)status;
  if (error) {
    throw std::runtime_error("failed to read permissions for: " +
                             path.string());
  }

  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add, error);
  if (error) {
    throw std::runtime_error("failed to set executable bit for: " +
                             path.string());
  }
#endif
}

}  // namespace zfleet::test
