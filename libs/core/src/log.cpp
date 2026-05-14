#include "zfleet/core/log.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zfleet::core::log {
namespace {

std::mutex& logger_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::shared_ptr<spdlog::logger>& logger_instance() {
  static auto logger = std::shared_ptr<spdlog::logger>{};
  return logger;
}

std::atomic<Level>& configured_level() {
  static auto level = std::atomic<Level>{Level::kInfo};
  return level;
}

spdlog::level::level_enum ToSpdlogLevel(Level level) {
  switch (level) {
    case Level::kTrace:
      return spdlog::level::trace;
    case Level::kDebug:
      return spdlog::level::debug;
    case Level::kInfo:
      return spdlog::level::info;
    case Level::kWarn:
      return spdlog::level::warn;
    case Level::kError:
      return spdlog::level::err;
    case Level::kCritical:
      return spdlog::level::critical;
    case Level::kOff:
      return spdlog::level::off;
  }

  return spdlog::level::info;
}

std::string NormalizeLevelText(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const char ch : text) {
    normalized.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return normalized;
}

void EnsureInitialized() {
  if (logger_instance() != nullptr) {
    return;
  }

  Init(Config{});
}

std::string BuildMessage(std::string_view context, std::string_view message) {
  if (context.empty()) {
    return std::string(message);
  }

  std::string output;
  output.reserve(context.size() + message.size() + 3);
  output.append("[");
  output.append(context);
  output.append("] ");
  output.append(message);
  return output;
}

} // namespace

Context::Context(std::string_view component)
    : component_(component), rendered_(component) {}

Context::Context(std::string component, std::string rendered)
    : component_(std::move(component)), rendered_(std::move(rendered)) {}

Context Context::With(Field field) const {
  std::string rendered = rendered_;
  if (!rendered.empty()) {
    rendered.push_back(' ');
  }
  rendered.append(field.key);
  rendered.push_back('=');
  rendered.append(field.value);
  return Context(component_, std::move(rendered));
}

Context Context::With(std::initializer_list<Field> fields) const {
  auto context = *this;
  for (const auto& field : fields) {
    context = context.With(field);
  }
  return context;
}

std::string_view Context::component() const noexcept {
  return component_;
}

std::string_view Context::Render() const noexcept {
  return rendered_;
}

void Init(const Config& config) {
  std::vector<spdlog::sink_ptr> sinks;
  sinks.reserve(2);

  if (config.enable_console) {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }

  if (!config.file_path.empty()) {
    if (config.file_path.has_parent_path()) {
      std::filesystem::create_directories(config.file_path.parent_path());
    }
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        config.file_path.string(), config.max_file_size, config.max_files, true));
  }

  if (sinks.empty()) {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }

  auto logger =
      std::make_shared<spdlog::logger>("zfleet", sinks.begin(), sinks.end());
  logger->set_pattern(
      "%Y-%m-%d %H:%M:%S.%e [%^%l%$] [pid=%P tid=%t] %v (%s:%#)");
  logger->set_level(ToSpdlogLevel(config.level));
  if (config.flush_on_warn_or_higher) {
    logger->flush_on(spdlog::level::warn);
  }

  std::lock_guard<std::mutex> lock(logger_mutex());
  logger_instance() = std::move(logger);
  configured_level().store(config.level);
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(logger_mutex());
  if (logger_instance() != nullptr) {
    logger_instance()->flush();
    logger_instance().reset();
  }
}

void SetLevel(Level level) {
  configured_level().store(level);
  std::lock_guard<std::mutex> lock(logger_mutex());
  if (logger_instance() != nullptr) {
    logger_instance()->set_level(ToSpdlogLevel(level));
  }
}

Level GetLevel() noexcept {
  return configured_level().load();
}

bool ShouldLog(Level level) noexcept {
  return level >= configured_level().load() && level != Level::kOff;
}

Context Component(std::string_view name) {
  return Context(name);
}

void Write(Level level,
           const Context& context,
           std::string_view message,
           std::source_location location) {
  EnsureInitialized();

  std::shared_ptr<spdlog::logger> logger;
  {
    std::lock_guard<std::mutex> lock(logger_mutex());
    logger = logger_instance();
  }

  if (logger == nullptr || !logger->should_log(ToSpdlogLevel(level))) {
    return;
  }

  const auto formatted_message = BuildMessage(context.Render(), message);
  logger->log(spdlog::source_loc(location.file_name(),
                                 static_cast<int>(location.line()),
                                 location.function_name()),
              ToSpdlogLevel(level), formatted_message);
}

Level ParseLevel(std::string_view text) {
  const auto normalized = NormalizeLevelText(text);
  if (normalized == "trace") {
    return Level::kTrace;
  }
  if (normalized == "debug") {
    return Level::kDebug;
  }
  if (normalized == "info") {
    return Level::kInfo;
  }
  if (normalized == "warn" || normalized == "warning") {
    return Level::kWarn;
  }
  if (normalized == "error" || normalized == "err") {
    return Level::kError;
  }
  if (normalized == "critical") {
    return Level::kCritical;
  }
  if (normalized == "off") {
    return Level::kOff;
  }

  throw std::invalid_argument("invalid log level: " + std::string(text));
}

std::string_view ToString(Level level) noexcept {
  switch (level) {
    case Level::kTrace:
      return "trace";
    case Level::kDebug:
      return "debug";
    case Level::kInfo:
      return "info";
    case Level::kWarn:
      return "warn";
    case Level::kError:
      return "error";
    case Level::kCritical:
      return "critical";
    case Level::kOff:
      return "off";
  }

  return "info";
}

} // namespace zfleet::core::log
