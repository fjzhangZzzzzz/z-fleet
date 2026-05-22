#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace zfleet::transport {

inline constexpr std::string_view kControlEventsPath = "/v1/control/events";
inline constexpr std::string_view kControlCommandsPath = "/v1/control/commands";
inline constexpr std::string_view kProtobufContentType =
    "application/x-protobuf";
inline constexpr std::uint32_t kDefaultMaxFrameBytes = 1024 * 1024;

std::vector<std::uint8_t> EncodeFrame(std::span<const std::uint8_t> payload);

class FrameDecoder {
 public:
  explicit FrameDecoder(std::uint32_t max_frame_bytes =
                            kDefaultMaxFrameBytes);

  std::vector<std::vector<std::uint8_t>> Push(
      std::span<const std::uint8_t> bytes);

 private:
  std::uint32_t max_frame_bytes_;
  std::vector<std::uint8_t> buffer_;
};

} // namespace zfleet::transport
