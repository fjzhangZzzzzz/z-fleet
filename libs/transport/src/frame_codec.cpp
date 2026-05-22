#include "zfleet/transport/frame_codec.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace zfleet::transport {
namespace {

constexpr std::size_t kLengthPrefixBytes = 4;

std::uint32_t ReadBigEndianLength(const std::vector<std::uint8_t>& buffer) {
  return (static_cast<std::uint32_t>(buffer[0]) << 24) |
         (static_cast<std::uint32_t>(buffer[1]) << 16) |
         (static_cast<std::uint32_t>(buffer[2]) << 8) |
         static_cast<std::uint32_t>(buffer[3]);
}

} // namespace

std::vector<std::uint8_t> EncodeFrame(std::span<const std::uint8_t> payload) {
  if (payload.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("protobuf frame is too large");
  }

  const auto length = static_cast<std::uint32_t>(payload.size());
  std::vector<std::uint8_t> frame;
  frame.reserve(kLengthPrefixBytes + payload.size());
  frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xff));
  frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xff));
  frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
  frame.push_back(static_cast<std::uint8_t>(length & 0xff));
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

FrameDecoder::FrameDecoder(std::uint32_t max_frame_bytes)
    : max_frame_bytes_(max_frame_bytes) {
  if (max_frame_bytes_ == 0) {
    throw std::invalid_argument("max frame size must be positive");
  }
}

std::vector<std::vector<std::uint8_t>> FrameDecoder::Push(
    std::span<const std::uint8_t> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

  std::vector<std::vector<std::uint8_t>> frames;
  while (buffer_.size() >= kLengthPrefixBytes) {
    const auto length = ReadBigEndianLength(buffer_);
    if (length > max_frame_bytes_) {
      throw std::runtime_error("protobuf frame exceeds maximum size");
    }

    const auto frame_size = kLengthPrefixBytes + static_cast<std::size_t>(length);
    if (buffer_.size() < frame_size) {
      break;
    }

    frames.emplace_back(buffer_.begin() + kLengthPrefixBytes,
                        buffer_.begin() + frame_size);
    buffer_.erase(buffer_.begin(), buffer_.begin() + frame_size);
  }

  return frames;
}

} // namespace zfleet::transport
