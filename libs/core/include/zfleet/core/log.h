#pragma once

#include <filesystem>
#include <initializer_list>
#include <source_location>
#include <string>
#include <string_view>

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

Level ParseLevel(std::string_view text);
std::string_view ToString(Level level) noexcept;

} // namespace zfleet::core::log
