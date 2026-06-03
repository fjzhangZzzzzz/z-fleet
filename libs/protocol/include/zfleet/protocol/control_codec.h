#pragma once

#include "zfleet/protocol/message.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zfleet::protocol {

std::vector<std::uint8_t> EncodeAgentEventPayload(const AgentEvent& event);
AgentEvent DecodeAgentEventPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeServerCommandPayload(
    const ServerCommand& command);
ServerCommand DecodeServerCommandPayload(std::span<const std::uint8_t> bytes);

std::string SerializeAgentEventBlob(const AgentEvent& event);
std::string SerializeTaskInputBlob(TaskType task_type, const TaskInput& input);
TaskInput ParseTaskInputBlob(TaskType task_type, const std::string& input_blob);
std::string SerializeTaskResultDataBlob(const TaskResultData& result);
std::string SerializeTaskErrorBlob(const TaskError& error);

}  // namespace zfleet::protocol
