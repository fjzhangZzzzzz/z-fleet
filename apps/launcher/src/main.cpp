#include "launcher.h"

#include <array>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::filesystem::path FallbackExecutablePath(int argc, char** argv) {
  return argc > 0 ? std::filesystem::absolute(argv[0])
                  : std::filesystem::current_path();
}

std::filesystem::path CurrentExecutablePath(int argc, char** argv) {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const auto size =
        GetModuleFileNameW(nullptr, buffer.data(),
                           static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return FallbackExecutablePath(argc, argv);
    }
    if (size < buffer.size()) {
      buffer.resize(size);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  std::array<char, 4096> buffer{};
  const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (size <= 0 || static_cast<std::size_t>(size) >= buffer.size()) {
    return FallbackExecutablePath(argc, argv);
  }
  return std::filesystem::path(std::string(buffer.data(),
                                           static_cast<std::size_t>(size)));
#endif
}

} // namespace

int main(int argc, char** argv) {
  const auto launcher_path = CurrentExecutablePath(argc, argv);
  return zfleet::launcher::RunLauncher(launcher_path, argc, argv);
}
