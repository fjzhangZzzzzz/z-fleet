#include "zfleet/core/time.h"

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace zfleet::core {

std::string NowUtcRfc3339() {
  const auto now = std::chrono::system_clock::now();
  const auto now_time_t = std::chrono::system_clock::to_time_t(now);

  std::tm utc_time{};
#if defined(_WIN32)
  gmtime_s(&utc_time, &now_time_t);
#else
  gmtime_r(&now_time_t, &utc_time);
#endif

  std::ostringstream stream;
  stream << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

} // namespace zfleet::core
