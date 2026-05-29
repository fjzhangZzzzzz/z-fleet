#include "launcher.h"

#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::filesystem::path CurrentExecutablePath(int argc, char* argv[]) {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                   static_cast<DWORD>(buffer.size()));
  if (length == 0) {
    throw std::runtime_error("failed to resolve current executable path");
  }
  while (length == buffer.size()) {
    buffer.resize(buffer.size() * 2);
    length = GetModuleFileNameW(nullptr, buffer.data(),
                                static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      throw std::runtime_error("failed to resolve current executable path");
    }
  }
  buffer.resize(length);
  return std::filesystem::path(buffer);
#else
  if (argc > 0 && argv[0] != nullptr) {
    return std::filesystem::canonical(argv[0]);
  }
  throw std::runtime_error("failed to resolve current executable path");
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto launcher_path = CurrentExecutablePath(argc, argv);
  return zfleet::launcher::RunLauncher(launcher_path, argc, argv);
}
