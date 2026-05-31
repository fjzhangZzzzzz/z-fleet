#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

void WriteLines(const std::filesystem::path& path, int argc, char* argv[]) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  for (int index = 1; index < argc; ++index) {
    const std::string_view value = argv[index] == nullptr ? "" : argv[index];
    stream << value << '\n';
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* args_file_env = std::getenv("ZFLEET_TEST_ARGS_FILE");
  const char* env_file_env = std::getenv("ZFLEET_TEST_ENV_FILE");
  if (args_file_env == nullptr || env_file_env == nullptr) {
    std::cerr << "missing test output env vars\n";
    return 2;
  }

  const char* component_root_env = std::getenv("ZFLEET_COMPONENT_ROOT");
  if (component_root_env == nullptr) {
    std::cerr << "missing ZFLEET_COMPONENT_ROOT\n";
    return 3;
  }

  WriteLines(std::filesystem::path(args_file_env), argc, argv);
  std::ofstream env_stream(std::filesystem::path(env_file_env),
                           std::ios::binary | std::ios::trunc);
  env_stream << component_root_env << '\n';

  return 23;
}
