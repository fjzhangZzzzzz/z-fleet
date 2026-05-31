#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::string_view GetOption(int argc, char* argv[], std::string_view name,
                           std::string_view default_value = {}) {
  const std::string prefix = std::string(name) + "=";
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index] == nullptr ? "" : argv[index];
    if (argument.starts_with(prefix)) {
      return argument.substr(prefix.size());
    }
  }
  return default_value;
}

int GetExitCode(int argc, char* argv[]) {
  const auto value = GetOption(argc, argv, "--code", "0");
  return std::atoi(std::string(value).c_str());
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto mode = GetOption(argc, argv, "--mode", "exit");

  if (mode == "exit") {
    return GetExitCode(argc, argv);
  }

  if (mode == "io") {
    std::cout << "stdout-line\n";
    std::cerr << "stderr-line\n";
    return GetExitCode(argc, argv);
  }

  if (mode == "echo-stdin") {
    std::string input;
    std::string chunk;
    while (std::getline(std::cin, chunk)) {
      input += chunk;
      if (!std::cin.eof()) {
        input.push_back('\n');
      }
    }
    std::cout << input;
    std::cerr << "stdin-consumed\n";
    return 0;
  }

  if (mode == "sleep") {
    const auto milliseconds = std::chrono::milliseconds(
        std::atoi(std::string(GetOption(argc, argv, "--ms", "1000")).c_str()));
    std::this_thread::sleep_for(milliseconds);
    return 0;
  }

  if (mode == "env") {
    const auto name = GetOption(argc, argv, "--name", "");
    const char* value = std::getenv(std::string(name).c_str());
    std::cout << (value == nullptr ? "" : value) << '\n';
    return 0;
  }

  std::cerr << "unknown mode\n";
  return 2;
}
