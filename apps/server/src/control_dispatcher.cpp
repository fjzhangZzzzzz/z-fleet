#include "control_dispatcher.h"

#include <exception>
#include <utility>
#include <variant>

#include "zfleet/protocol/control_codec.h"

namespace zfleet::server {
namespace {

ControlEventResult InvalidArgument(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kInvalidArgument,
      .message = std::move(message),
  };
}

}  // namespace

ControlDispatcher::ControlDispatcher(const ControlService* service)
    : service_(service), registry_(nullptr) {}

ControlDispatcher::ControlDispatcher(const ControlService* service,
                                     ControlConnectionRegistry* registry,
                                     std::string connection_id)
    : service_(service),
      registry_(registry),
      connection_id_(std::move(connection_id)) {}

std::vector<ControlEventResult> ControlDispatcher::PushEventBytes(
    std::span<const std::uint8_t> bytes) {
  std::vector<std::vector<std::uint8_t>> frames;
  try {
    frames = decoder_.Push(bytes);
  } catch (const std::exception& ex) {
    return {InvalidArgument(ex.what())};
  }

  std::vector<zfleet::protocol::AgentEvent> events;
  events.reserve(frames.size());
  for (const auto& frame : frames) {
    try {
      events.push_back(zfleet::protocol::DecodeAgentEventPayload(
          std::span<const std::uint8_t>{frame.data(), frame.size()}));
    } catch (const std::exception& ex) {
      events.clear();
      return {InvalidArgument(ex.what())};
    }
  }

  std::vector<ControlEventResult> results;
  results.reserve(events.size());
  for (const auto& event : events) {
    auto result = service_->HandleAgentEvent(event);
    if (result.status == ControlEventStatus::kAccepted) {
      RecordAcceptedEvent(event);
    }
    results.push_back(std::move(result));
  }
  return results;
}

void ControlDispatcher::RecordAcceptedEvent(
    const zfleet::protocol::AgentEvent& event) const {
  if (registry_ == nullptr || connection_id_.empty()) {
    return;
  }

  if (std::holds_alternative<zfleet::protocol::AgentRegistration>(
          event.payload)) {
    registry_->BindAgent(
        connection_id_,
        std::string(zfleet::protocol::AgentEventAgentId(event)),
        std::string(zfleet::protocol::AgentEventOccurredAt(event)));
  } else if (std::holds_alternative<zfleet::protocol::AgentHeartbeat>(
                 event.payload)) {
    registry_->RecordHeartbeat(
        connection_id_,
        std::string(zfleet::protocol::AgentEventAgentId(event)),
        std::string(zfleet::protocol::AgentEventOccurredAt(event)));
  }
}

}  // namespace zfleet::server
