#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zfleet::platform {

enum class ProcessStreamMode {
  kInherit,
  kNull,
  kPipe,
};

struct ProcessStreamConfig {
  ProcessStreamMode mode = ProcessStreamMode::kInherit;
};

struct ProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> args;
  std::vector<std::string> env;
  std::optional<std::filesystem::path> working_directory;
  ProcessStreamConfig stdin_config;
  ProcessStreamConfig stdout_config;
  ProcessStreamConfig stderr_config;
};

struct ProcessWaitOptions {
  std::optional<std::chrono::milliseconds> timeout;
};

struct ProcessExitStatus {
  bool exited = false;
  int exit_code = -1;
  bool timed_out = false;
};

struct ProcessOutput {
  ProcessExitStatus status;
  std::string stdout_data;
  std::string stderr_data;
};

class Process {
 public:
  Process() = default;
  Process(Process&& other) noexcept;
  Process& operator=(Process&& other) noexcept;
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  ~Process();

  static Process Spawn(const ProcessOptions& options);

  bool Valid() const noexcept;
  int Id() const noexcept;

  void WriteStdin(std::string_view data);
  void CloseStdin();
  void Detach();

  ProcessExitStatus Wait(const ProcessWaitOptions& options = {});
  ProcessOutput WaitWithOutput(const ProcessWaitOptions& options = {});

  bool Kill();
  bool Terminate();

 private:
  struct Impl;
  explicit Process(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

ProcessExitStatus Run(const ProcessOptions& options,
                      const ProcessWaitOptions& wait_options = {});
ProcessOutput Output(const ProcessOptions& options,
                     const ProcessWaitOptions& wait_options = {});

}  // namespace zfleet::platform
