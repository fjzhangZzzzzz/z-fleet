#include "control_service.h"
#include "database.h"

#include "test_util.h"
#include "zfleet/crypto/sha256.h"
#include "zfleet/protocol/v1/agent_control.pb.h"
#include "server_event_test_util.h"
#include "sqlite_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace {

namespace proto = zfleet::protocol::v1;

using zfleet::test::AssetSnapshotEvent;
using zfleet::test::CountRows;
using zfleet::test::HeartbeatEvent;
using zfleet::test::ReadAgentField;
using zfleet::test::ReadAuditField;
using zfleet::test::ReadTaskField;
using zfleet::test::ReadTaskResultBlob;
using zfleet::test::ReadTaskResultField;
using zfleet::test::RegisterEvent;
using zfleet::test::SeedAgent;
using zfleet::test::SeedTask;
using zfleet::test::TaskFailedEvent;
using zfleet::test::TaskRunningEvent;
using zfleet::test::TaskSucceededEvent;
TEST_CASE("control service stores task running and result events") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  SeedTask(&database, "task-1", "agent-1");
  REQUIRE(database.ClaimNextTaskForAgent("agent-1", "2026-05-21T10:00:01Z")
              .has_value());
  const zfleet::server::ControlService service(&database);

  const auto running_result = service.HandleAgentEvent(TaskRunningEvent(
      "task-running-1", "agent-1", "task-1", "2026-05-21T10:00:02Z"));
  const auto result_result = service.HandleAgentEvent(TaskSucceededEvent(
      "task-result-1", "agent-1", "task-1", "2026-05-21T10:00:03Z"));

  REQUIRE(running_result.status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(running_result.message == "running");
  REQUIRE(result_result.status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(result_result.message == "accepted");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "succeeded");
  REQUIRE(ReadTaskField(database_path, "task-1", "completed_at") ==
          "2026-05-21T10:00:03Z");
  REQUIRE(CountRows(database_path, "task_results") == 1);
  REQUIRE(ReadAuditField(database_path, "task-1", "event_type") ==
          "task.assigned");
  REQUIRE(ReadAuditField(database_path, "task-1", "result") == "success");
  REQUIRE(ReadAuditField(database_path, "task-running-1", "event_type") ==
          "task.running");
  REQUIRE(ReadAuditField(database_path, "task-running-1", "result") ==
          "success");
  REQUIRE(ReadAuditField(database_path, "task-result-1", "event_type") ==
          "task.succeeded");
  REQUIRE(ReadAuditField(database_path, "task-result-1", "result") ==
          "success");
  proto::CollectBasicInventoryResult stored_result;
  REQUIRE(stored_result.ParseFromString(
      ReadTaskResultBlob(database_path, "task-1", "result_blob")));
  REQUIRE(stored_result.hostname() == "devbox-01");
  REQUIRE(ReadTaskResultBlob(database_path, "task-1", "error_blob").empty());

  const auto repeated_result = service.HandleAgentEvent(TaskSucceededEvent(
      "task-result-1-repeat", "agent-1", "task-1", "2026-05-21T10:00:04Z"));
  REQUIRE(repeated_result.status ==
          zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(repeated_result.message == "task already finished");
  REQUIRE(CountRows(database_path, "task_results") == 1);
}

TEST_CASE("control service stores failed task error columns and blob") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  SeedTask(&database, "task-failed-1", "agent-1");
  REQUIRE(database.ClaimNextTaskForAgent("agent-1", "2026-05-21T10:00:01Z")
              .has_value());
  const zfleet::server::ControlService service(&database);

  REQUIRE(service
              .HandleAgentEvent(TaskRunningEvent("task-running-failed-1",
                                                 "agent-1", "task-failed-1",
                                                 "2026-05-21T10:00:02Z"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  const auto result = service.HandleAgentEvent(
      TaskFailedEvent("task-result-failed-1", "agent-1", "task-failed-1",
                      "2026-05-21T10:00:03Z"));

  REQUIRE(result.status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadTaskField(database_path, "task-failed-1", "state") == "failed");
  REQUIRE(ReadTaskResultField(database_path, "task-failed-1", "error_code") ==
          "internal_error");
  REQUIRE(ReadTaskResultField(database_path, "task-failed-1",
                              "error_retryable") == "1");
  proto::AgentError stored_error;
  REQUIRE(stored_error.ParseFromString(
      ReadTaskResultBlob(database_path, "task-failed-1", "error_blob")));
  REQUIRE(stored_error.retryable());
  REQUIRE(stored_error.message() == "inventory failed");
  REQUIRE(ReadAuditField(database_path, "task-running-failed-1",
                         "event_type") == "task.running");
  REQUIRE(ReadAuditField(database_path, "task-running-failed-1", "result") ==
          "success");
  REQUIRE(ReadAuditField(database_path, "task-result-failed-1", "event_type") ==
          "task.failed");
  REQUIRE(ReadAuditField(database_path, "task-result-failed-1", "result") ==
          "success");
}

TEST_CASE("control service registers agent and accepts heartbeat") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::ControlService service(&database);

  REQUIRE(service
              .HandleAgentEvent(RegisterEvent("register-1", "agent-1",
                                              "2026-05-21T10:00:00Z"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(service
              .HandleAgentEvent(HeartbeatEvent("heartbeat-1", "agent-1",
                                               "2026-05-21T10:05:00Z"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(database.AgentExists("agent-1"));
  REQUIRE(CountRows(database_path, "audit_events") == 1);
}

TEST_CASE("control service stores asset snapshot display columns and blob") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  const zfleet::server::ControlService service(&database);

  REQUIRE(service
              .HandleAgentEvent(AssetSnapshotEvent("asset-1", "agent-1",
                                                   "2026-05-21T10:10:00Z"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  const auto latest = database.FindLatestAssetSnapshot("agent-1");
  REQUIRE(latest.has_value());
  REQUIRE(latest->hostname == "devbox-01");
  REQUIRE(latest->applications == std::vector<std::string>{"cmake"});
  REQUIRE(latest->services == std::vector<std::string>{"zfleet-agent"});
}

TEST_CASE("control service consumes scoped registration tokens") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.CreateRegistrationToken(zfleet::server::RegistrationTokenRecord{
      .token_id = "token-1",
      .token_hash = zfleet::crypto::Sha256BytesHex("register-once"),
      .purpose = "agent_register",
      .channel = std::optional<std::string>{"stable"},
      .platform = std::optional<std::string>{"linux"},
      .arch = std::optional<std::string>{"x86_64"},
      .max_uses = 1,
      .use_count = 0,
      .status = "active",
      .created_at = "2026-05-21T09:00:00Z",
      .expires_at = "2099-05-21T09:00:00Z",
  });
  const zfleet::server::ControlService service(&database);

  REQUIRE(service
              .HandleAgentEvent(
                  RegisterEvent("token-register-1", "agent-token-1",
                                "2026-05-21T10:00:00Z", "register-once"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(service
              .HandleAgentEvent(
                  RegisterEvent("token-register-2", "agent-token-2",
                                "2026-05-21T10:00:01Z", "register-once"))
              .status == zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(database.AgentExists("agent-token-1"));
  REQUIRE_FALSE(database.AgentExists("agent-token-2"));
  REQUIRE(database.ListRegistrationTokens().front().status == "consumed");
  REQUIRE(database.ListRegistrationTokens().front().use_count == 1);
}

TEST_CASE("control service rejects invalid and unregistered events") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::ControlService service(&database);

  proto::AgentEvent missing_payload;
  missing_payload.set_protocol_version("v1");
  missing_payload.set_message_id("http2-missing-payload");
  missing_payload.set_agent_id("agent-1");
  missing_payload.set_occurred_at("2026-05-21T10:00:00Z");

  const auto missing_payload_result = service.HandleAgentEvent(missing_payload);
  const auto heartbeat_result = service.HandleAgentEvent(HeartbeatEvent(
      "http2-unregistered-heartbeat", "agent-1", "2026-05-21T10:00:05Z"));

  REQUIRE(missing_payload_result.status ==
          zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(missing_payload_result.message == "event payload must be set");
  REQUIRE(heartbeat_result.status ==
          zfleet::server::ControlEventStatus::kNotFound);
  REQUIRE(heartbeat_result.message == "agent not registered");
  REQUIRE(CountRows(database_path, "audit_events") == 0);
}

}  // namespace
