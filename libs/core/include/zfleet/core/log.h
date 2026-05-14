#pragma once

#include <fmt/format.h>

#include <filesystem>
#include <initializer_list>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace zfleet::core::log {

enum class Level {
  kTrace,
  kDebug,
  kInfo,
  kWarn,
  kError,
  kCritical,
  kOff,
};

struct Config {
  Level level = Level::kInfo;
  bool enable_console = true;
  std::filesystem::path file_path;
  std::size_t max_file_size = 5 * 1024 * 1024;
  std::size_t max_files = 3;
  bool flush_on_warn_or_higher = true;
};

struct Field {
  std::string_view key;
  std::string_view value;
};

class Context {
 public:
  Context() = default;
  explicit Context(std::string_view component);

  Context With(Field field) const;
  Context With(std::initializer_list<Field> fields) const;

  std::string_view component() const noexcept;
  std::string_view Render() const noexcept;

 private:
  explicit Context(std::string component, std::string rendered);

  std::string component_;
  std::string rendered_;
};

void Init(const Config& config);
void Shutdown();

void SetLevel(Level level);
Level GetLevel() noexcept;
bool ShouldLog(Level level) noexcept;

Context Component(std::string_view name);

void Write(
    Level level,
    const Context& context,
    std::string_view message,
    std::source_location location = std::source_location::current());

template <typename... Args>
void LogAt(
    Level level,
    const Context& context,
    std::source_location location,
    fmt::format_string<Args...> format,
    Args&&... args) {
  if (!ShouldLog(level)) {
    return;
  }

  Write(level, context, fmt::format(format, std::forward<Args>(args)...),
        location);
}

template <typename... Args>
void Log(
    Level level,
    const Context& context,
    fmt::format_string<Args...> format,
    Args&&... args) {
  LogAt(level, context, std::source_location::current(), format,
        std::forward<Args>(args)...);
}

Level ParseLevel(std::string_view text);
std::string_view ToString(Level level) noexcept;

} // namespace zfleet::core::log

#define ZFLOG_TRACE(ctx, fmt, ...) \
  ::zfleet::core::log::LogAt(::zfleet::core::log::Level::kTrace, (ctx), \
                             std::source_location::current(), (fmt), \
                             ##__VA_ARGS__)

#define ZFLOG_DEBUG(ctx, fmt, ...) \
  ::zfleet::core::log::LogAt(::zfleet::core::log::Level::kDebug, (ctx), \
                             std::source_location::current(), (fmt), \
                             ##__VA_ARGS__)

#define ZFLOG_INFO(ctx, fmt, ...) \
  ::zfleet::core::log::LogAt(::zfleet::core::log::Level::kInfo, (ctx), \
                             std::source_location::current(), (fmt), \
                             ##__VA_ARGS__)

#define ZFLOG_WARN(ctx, fmt, ...) \
  ::zfleet::core::log::LogAt(::zfleet::core::log::Level::kWarn, (ctx), \
                             std::source_location::current(), (fmt), \
                             ##__VA_ARGS__)

#define ZFLOG_ERROR(ctx, fmt, ...) \
  ::zfleet::core::log::LogAt(::zfleet::core::log::Level::kError, (ctx), \
                             std::source_location::current(), (fmt), \
                             ##__VA_ARGS__)

#define ZFLOG_CRITICAL(ctx, fmt, ...) \
  ::zfleet::core::log::LogAt(::zfleet::core::log::Level::kCritical, (ctx), \
                             std::source_location::current(), (fmt), \
                             ##__VA_ARGS__)
