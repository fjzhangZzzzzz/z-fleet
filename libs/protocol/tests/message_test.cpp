#include "zfleet/protocol/json_codec.h"
#include "zfleet/protocol/message.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using zfleet::protocol::AuditEventType;
using zfleet::protocol::CapabilityLevel;
using zfleet::protocol::ErrorCode;
using zfleet::protocol::TaskExecutionStatus;
using zfleet::protocol::TaskState;
using zfleet::protocol::TaskType;

} // namespace

TEST_CASE("protocol metadata and enum conversions are available") {
  REQUIRE(zfleet::protocol::protocol_version() == "v1");
  REQUIRE(zfleet::protocol::ToString(ErrorCode::agent_id_mismatch) ==
          "agent_id_mismatch");
  REQUIRE(zfleet::protocol::ToString(ErrorCode::task_execution_failed) ==
          "task_execution_failed");
  REQUIRE(zfleet::protocol::ToString(AuditEventType::agent_asset_snapshot) ==
          "agent.asset_snapshot");
  REQUIRE(zfleet::protocol::ToString(AuditEventType::task_assigned) ==
          "task.assigned");
  REQUIRE(zfleet::protocol::ToString(TaskType::collect_basic_inventory) ==
          "collect_basic_inventory");
  REQUIRE(zfleet::protocol::ToString(CapabilityLevel::readonly) == "readonly");
  REQUIRE(zfleet::protocol::ToString(TaskExecutionStatus::expired) ==
          "expired");
  REQUIRE(zfleet::protocol::ToString(TaskState::running) == "running");
  REQUIRE(zfleet::protocol::ErrorCodeFromString("internal_error") ==
          ErrorCode::internal_error);
  REQUIRE(zfleet::protocol::ErrorCodeFromString("task_not_found") ==
          ErrorCode::task_not_found);
  REQUIRE(zfleet::protocol::AuditEventTypeFromString("agent.register") ==
          AuditEventType::agent_register);
  REQUIRE(zfleet::protocol::AuditEventTypeFromString("task.expired") ==
          AuditEventType::task_expired);
  REQUIRE(zfleet::protocol::TaskTypeFromString("collect_basic_inventory") ==
          TaskType::collect_basic_inventory);
  REQUIRE(zfleet::protocol::CapabilityLevelFromString("shell") ==
          CapabilityLevel::shell);
  REQUIRE(zfleet::protocol::TaskExecutionStatusFromString("failed") ==
          TaskExecutionStatus::failed);
  REQUIRE(zfleet::protocol::TaskStateFromString("queued") ==
          TaskState::queued);
}

TEST_CASE("audit payload supports compact json serialization") {
  const auto payload = zfleet::protocol::SerializeAuditPayload(
      {{"transport", std::string("http2")},
       {"retryable", false},
       {"attempt", std::int64_t{3}}});

  REQUIRE(payload.find("\"transport\":\"http2\"") != std::string::npos);
  REQUIRE(payload.find("\"retryable\":false") != std::string::npos);
  REQUIRE(payload.find("\"attempt\":3") != std::string::npos);
}
