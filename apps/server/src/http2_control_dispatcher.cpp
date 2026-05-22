#include "http2_control_dispatcher.h"

#include "zfleet/protocol/v1/agent_control.pb.h"

#include <exception>
#include <utility>

namespace zfleet::server {
namespace {

ControlEventResult InvalidArgument(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kInvalidArgument,
      .message = std::move(message),
  };
}

} // namespace

Http2ControlDispatcher::Http2ControlDispatcher(Http2ControlService* service)
    : service_(service) {}

std::vector<ControlEventResult> Http2ControlDispatcher::PushEventBytes(
    std::span<const std::uint8_t> bytes) {
  std::vector<std::vector<std::uint8_t>> frames;
  try {
    frames = decoder_.Push(bytes);
  } catch (const std::exception& ex) {
    return {InvalidArgument(ex.what())};
  }

  std::vector<ControlEventResult> results;
  results.reserve(frames.size());
  for (const auto& frame : frames) {
    zfleet::protocol::v1::AgentEvent event;
    if (!event.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
      results.push_back(InvalidArgument("invalid protobuf agent event"));
      continue;
    }
    results.push_back(service_->HandleAgentEvent(event));
  }
  return results;
}

} // namespace zfleet::server
