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

Http2ControlDispatcher::Http2ControlDispatcher(
    const Http2ControlService* service)
    : service_(service), registry_(nullptr) {}

Http2ControlDispatcher::Http2ControlDispatcher(
    const Http2ControlService* service,
    Http2ConnectionRegistry* registry,
    std::string connection_id)
    : service_(service),
      registry_(registry),
      connection_id_(std::move(connection_id)) {}

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
    auto result = service_->HandleAgentEvent(event);
    if (result.status == ControlEventStatus::kAccepted) {
      RecordAcceptedEvent(event);
    }
    results.push_back(std::move(result));
  }
  return results;
}

void Http2ControlDispatcher::RecordAcceptedEvent(
    const zfleet::protocol::v1::AgentEvent& event) const {
  if (registry_ == nullptr || connection_id_.empty()) {
    return;
  }

  switch (event.payload_case()) {
    case zfleet::protocol::v1::AgentEvent::kRegister:
      registry_->BindAgent(connection_id_, event.agent_id(),
                           event.occurred_at());
      return;
    case zfleet::protocol::v1::AgentEvent::kHeartbeat:
      registry_->RecordHeartbeat(connection_id_, event.agent_id(),
                                 event.occurred_at());
      return;
    default:
      return;
  }
}

} // namespace zfleet::server
