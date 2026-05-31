#include "zfleet/platform/process.h"

#include <boost/process/v1/args.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/start_dir.hpp>

#include <csignal>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <thread>

namespace bp = boost::process::v1;

namespace zfleet::platform {
namespace {

bp::environment BuildEnvironment(const std::vector<std::string>& env) {
  bp::environment process_env = boost::this_process::environment();
  for (const auto& entry : env) {
    const auto separator = entry.find('=');
    if (separator == std::string::npos || separator == 0) {
      throw std::runtime_error("invalid process environment entry: " + entry);
    }
    process_env[entry.substr(0, separator)] = entry.substr(separator + 1);
  }
  return process_env;
}

bp::child SpawnChild(const ProcessOptions& options, bp::opstream* stdin_pipe,
                     bp::ipstream* stdout_pipe, bp::ipstream* stderr_pipe) {
  const auto environment = BuildEnvironment(options.env);
  const auto start_dir = options.working_directory.has_value()
                             ? options.working_directory->string()
                             : std::filesystem::current_path().string();

  const bool stdin_is_pipe =
      options.stdin_config.mode == ProcessStreamMode::kPipe;
  const bool stdout_is_pipe =
      options.stdout_config.mode == ProcessStreamMode::kPipe;
  const bool stderr_is_pipe =
      options.stderr_config.mode == ProcessStreamMode::kPipe;

  if (options.stdin_config.mode == ProcessStreamMode::kInherit &&
      options.stdout_config.mode == ProcessStreamMode::kInherit &&
      options.stderr_config.mode == ProcessStreamMode::kInherit) {
    return bp::child(options.executable.string(), bp::args = options.args,
                     bp::env = environment, bp::start_dir = start_dir);
  }

  if (options.stdin_config.mode == ProcessStreamMode::kNull && stdout_is_pipe &&
      stderr_is_pipe) {
    return bp::child(options.executable.string(), bp::args = options.args,
                     bp::env = environment, bp::start_dir = start_dir,
                     bp::std_in<bp::null, bp::std_out> * stdout_pipe,
                     bp::std_err > *stderr_pipe);
  }

  if (stdin_is_pipe && stdout_is_pipe && stderr_is_pipe) {
    return bp::child(options.executable.string(), bp::args = options.args,
                     bp::env = environment, bp::start_dir = start_dir,
                     bp::std_in<*stdin_pipe, bp::std_out> * stdout_pipe,
                     bp::std_err > *stderr_pipe);
  }

  if (stdin_is_pipe && options.stdout_config.mode == ProcessStreamMode::kNull &&
      options.stderr_config.mode == ProcessStreamMode::kNull) {
    return bp::child(options.executable.string(), bp::args = options.args,
                     bp::env = environment, bp::start_dir = start_dir,
                     bp::std_in<*stdin_pipe, bp::std_out> bp::null,
                     bp::std_err > bp::null);
  }

  if (options.stdin_config.mode == ProcessStreamMode::kNull &&
      options.stdout_config.mode == ProcessStreamMode::kNull &&
      options.stderr_config.mode == ProcessStreamMode::kNull) {
    return bp::child(options.executable.string(), bp::args = options.args,
                     bp::env = environment, bp::start_dir = start_dir,
                     bp::std_in<bp::null, bp::std_out> bp::null,
                     bp::std_err > bp::null);
  }

  throw std::runtime_error("unsupported process stream configuration");
}

ProcessExitStatus WaitForChild(bp::child* child,
                               const ProcessWaitOptions& options) {
  ProcessExitStatus status;
  if (child == nullptr) {
    return status;
  }
  if (!child->valid()) {
    status.exited = true;
    return status;
  }

  if (options.timeout.has_value()) {
    const auto deadline = std::chrono::steady_clock::now() + *options.timeout;
    while (true) {
      std::error_code error;
      if (!child->running(error)) {
        break;
      }
      if (error) {
        return status;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        status.timed_out = true;
        return status;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  } else {
    child->wait();
  }

  status.exited = true;
  status.exit_code = child->exit_code();
  return status;
}

std::string ReadStream(bp::ipstream* stream) {
  if (stream == nullptr) {
    return {};
  }

  std::ostringstream output;
  output << stream->rdbuf();
  return output.str();
}

bool IsRunning(bp::child* child) {
  if (child == nullptr || !child->valid()) {
    return false;
  }
  std::error_code error;
  return child->running(error);
}

}  // namespace

struct Process::Impl {
  explicit Impl(const ProcessOptions& options)
      : stdin_pipe(options.stdin_config.mode == ProcessStreamMode::kPipe
                       ? std::optional<bp::opstream>(std::in_place)
                       : std::nullopt),
        stdout_pipe(options.stdout_config.mode == ProcessStreamMode::kPipe
                        ? std::optional<bp::ipstream>(std::in_place)
                        : std::nullopt),
        stderr_pipe(options.stderr_config.mode == ProcessStreamMode::kPipe
                        ? std::optional<bp::ipstream>(std::in_place)
                        : std::nullopt),
        child(SpawnChild(options, stdin_pipe ? &*stdin_pipe : nullptr,
                         stdout_pipe ? &*stdout_pipe : nullptr,
                         stderr_pipe ? &*stderr_pipe : nullptr)) {}

  std::optional<bp::opstream> stdin_pipe;
  std::optional<bp::ipstream> stdout_pipe;
  std::optional<bp::ipstream> stderr_pipe;
  bp::child child;
};

Process::Process(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Process::Process(Process&& other) noexcept : impl_(std::move(other.impl_)) {}

Process& Process::operator=(Process&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  impl_ = std::move(other.impl_);
  return *this;
}

Process::~Process() = default;

Process Process::Spawn(const ProcessOptions& options) {
  return Process(std::make_unique<Impl>(options));
}

bool Process::Valid() const noexcept {
  return impl_ != nullptr && impl_->child.valid();
}

int Process::Id() const noexcept {
  if (!Valid()) {
    return -1;
  }
  return static_cast<int>(impl_->child.id());
}

void Process::WriteStdin(std::string_view data) {
  if (impl_ == nullptr || !impl_->stdin_pipe.has_value() ||
      !impl_->stdin_pipe->pipe().is_open()) {
    throw std::runtime_error("process stdin is not captured");
  }
  impl_->stdin_pipe->write(data.data(),
                           static_cast<std::streamsize>(data.size()));
  impl_->stdin_pipe->flush();
}

void Process::CloseStdin() {
  if (impl_ == nullptr || !impl_->stdin_pipe.has_value() ||
      !impl_->stdin_pipe->pipe().is_open()) {
    return;
  }
  impl_->stdin_pipe->close();
}

void Process::Detach() {
  if (impl_ == nullptr || !impl_->child.valid()) {
    return;
  }
  impl_->child.detach();
}

ProcessExitStatus Process::Wait(const ProcessWaitOptions& options) {
  if (impl_ == nullptr) {
    return {};
  }
  return WaitForChild(&impl_->child, options);
}

ProcessOutput Process::WaitWithOutput(const ProcessWaitOptions& options) {
  ProcessOutput output;
  if (impl_ == nullptr) {
    return output;
  }

  output.status = WaitForChild(&impl_->child, options);
  if (!output.status.exited) {
    return output;
  }

  output.stdout_data =
      ReadStream(impl_->stdout_pipe ? &*impl_->stdout_pipe : nullptr);
  output.stderr_data =
      ReadStream(impl_->stderr_pipe ? &*impl_->stderr_pipe : nullptr);
  return output;
}

bool Process::Kill() {
  if (!Valid() || !IsRunning(&impl_->child)) {
    return false;
  }
  impl_->child.terminate();
  impl_->child.wait();
  return true;
}

bool Process::Terminate() {
  if (!Valid() || !IsRunning(&impl_->child)) {
    return false;
  }
#ifdef _WIN32
  impl_->child.terminate();
  impl_->child.wait();
#else
  if (::kill(impl_->child.id(), SIGTERM) != 0) {
    return false;
  }
#endif
  return true;
}

ProcessExitStatus Run(const ProcessOptions& options,
                      const ProcessWaitOptions& wait_options) {
  auto process = Process::Spawn(options);
  return process.Wait(wait_options);
}

ProcessOutput Output(const ProcessOptions& options,
                     const ProcessWaitOptions& wait_options) {
  auto process_options = options;
  process_options.stdin_config.mode = ProcessStreamMode::kNull;
  process_options.stdout_config.mode = ProcessStreamMode::kPipe;
  process_options.stderr_config.mode = ProcessStreamMode::kPipe;

  auto process = Process::Spawn(process_options);
  return process.WaitWithOutput(wait_options);
}

}  // namespace zfleet::platform
