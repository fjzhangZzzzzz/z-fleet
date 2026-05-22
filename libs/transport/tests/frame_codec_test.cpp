#include "zfleet/transport/frame_codec.h"
#include "zfleet/transport/http2_session.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

TEST_CASE("protobuf frame codec handles partial input") {
  const std::vector<std::uint8_t> first{1, 2, 3};
  const std::vector<std::uint8_t> second{4, 5};

  const auto first_frame = zfleet::transport::EncodeFrame(first);
  const auto second_frame = zfleet::transport::EncodeFrame(second);

  zfleet::transport::FrameDecoder decoder;
  REQUIRE(decoder.Push(std::span<const std::uint8_t>{first_frame.data(), 2})
              .empty());

  std::vector<std::uint8_t> rest(first_frame.begin() + 2, first_frame.end());
  rest.insert(rest.end(), second_frame.begin(), second_frame.end());

  const auto frames = decoder.Push(
      std::span<const std::uint8_t>{rest.data(), rest.size()});
  REQUIRE(frames.size() == 2);
  REQUIRE(frames[0] == first);
  REQUIRE(frames[1] == second);
}

TEST_CASE("transport links nghttp2") {
  REQUIRE(zfleet::transport::LinkedNghttp2Version() > 0);
}
