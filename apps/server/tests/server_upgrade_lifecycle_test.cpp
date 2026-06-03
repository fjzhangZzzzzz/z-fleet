#include "database.h"
#include "control_service.h"

#include "test_util.h"
#include "server_event_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace {

using zfleet::test::ReadAgentField;
using zfleet::test::DomainAgentEvent;
using zfleet::test::RegisterEvent;
using zfleet::test::SeedAgent;

}  // namespace

TEST_CASE("agent reconnect confirms desired package version") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-upgrade-confirm");
  database.ScheduleAgentUpgrade(
      "agent-upgrade-confirm", "0.2.0", "pkg-confirm",
      std::optional<std::string>{"admin"}, "2026-05-24T10:00:00Z",
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-confirm",
          .agent_id = "agent-upgrade-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .capability_level =
              zfleet::protocol::CapabilityLevel::high_risk_write,
          .created_at = "2026-05-24T10:00:00Z",
          .expires_at = "2099-05-24T10:00:00Z",
          .input =
              zfleet::protocol::PackageUpdateInput{
                  .component = "agent",
                  .package_id = "pkg-confirm",
                  .version = "0.2.0",
              },
      });
  const zfleet::server::ControlService service(&database);
  REQUIRE(service
              .HandleAgentEvent(DomainAgentEvent(RegisterEvent(
                  "reconnect-before-apply", "agent-upgrade-confirm",
                  "2026-05-24T10:00:00Z")))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-confirm",
                         "upgrade_state") == "queued");
  REQUIRE(database
              .ClaimNextTaskForAgent("agent-upgrade-confirm",
                                     "2026-05-24T10:00:01Z")
              .has_value());
  database.RecordTaskResult(
      zfleet::protocol::TaskResult{
          .protocol_version = "v1",
          .request_id = "update-complete",
          .task_id = "task-confirm",
          .agent_id = "agent-upgrade-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .occurred_at = "2026-05-24T10:00:02Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result =
              zfleet::protocol::PackageUpdateResult{
                  .component = "agent",
                  .package_id = "pkg-confirm",
                  .version = "0.2.0",
                  .state = "applied",
              },
      },
      std::nullopt, std::nullopt);

  REQUIRE(service
              .HandleAgentEvent(
                  DomainAgentEvent(
                  RegisterEvent("reconnect-confirm", "agent-upgrade-confirm",
                                "2026-05-24T10:01:00Z", {}, "0.2.0")))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-confirm",
                         "upgrade_state") == "succeeded");
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-confirm",
                         "current_package_id") == "pkg-confirm");
}

TEST_CASE("agent rollback clears desired target and completes on reconnect") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-rollback-confirm");
  database.ScheduleAgentRollback(
      "agent-rollback-confirm", std::optional<std::string>{"admin"},
      "2026-05-24T10:00:00Z",
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-rollback",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .capability_level =
              zfleet::protocol::CapabilityLevel::high_risk_write,
          .created_at = "2026-05-24T10:00:00Z",
          .expires_at = "2099-05-24T10:00:00Z",
          .input =
              zfleet::protocol::PackageUpdateInput{
                  .action = "rollback",
                  .component = "agent",
              },
      });
  REQUIRE(
      ReadAgentField(database_path, "agent-rollback-confirm", "desired_version")
          .empty());
  REQUIRE(database
              .ClaimNextTaskForAgent("agent-rollback-confirm",
                                     "2026-05-24T10:00:01Z")
              .has_value());
  database.RecordTaskResult(
      zfleet::protocol::TaskResult{
          .protocol_version = "v1",
          .request_id = "rollback-complete",
          .task_id = "task-rollback",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .occurred_at = "2026-05-24T10:00:02Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result =
              zfleet::protocol::PackageUpdateResult{
                  .component = "agent",
                  .state = "applied",
              },
      },
      std::nullopt, std::nullopt);
  REQUIRE(ReadAgentField(database_path, "agent-rollback-confirm",
                         "upgrade_state") == "waiting_reconnect");

  const zfleet::server::ControlService service(&database);
  REQUIRE(service
              .HandleAgentEvent(DomainAgentEvent(RegisterEvent(
                  "reconnect-rollback", "agent-rollback-confirm",
                  "2026-05-24T10:01:00Z")))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentField(database_path, "agent-rollback-confirm",
                         "upgrade_state") == "succeeded");
}

TEST_CASE("agent rollback timeout marks waiting reconnect timeout") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-rollback-confirm");
  database.ScheduleAgentRollback(
      "agent-rollback-confirm", std::optional<std::string>{"admin"},
      "2026-05-24T11:00:00Z",
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-rollback-timeout",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .capability_level =
              zfleet::protocol::CapabilityLevel::high_risk_write,
          .created_at = "2026-05-24T11:00:00Z",
          .expires_at = "2099-05-24T10:00:00Z",
          .input =
              zfleet::protocol::PackageUpdateInput{
                  .action = "rollback",
                  .component = "agent",
              },
      });
  REQUIRE(database
              .ClaimNextTaskForAgent("agent-rollback-confirm",
                                     "2026-05-24T11:00:01Z")
              .has_value());
  database.RecordTaskResult(
      zfleet::protocol::TaskResult{
          .protocol_version = "v1",
          .request_id = "rollback-awaiting-timeout",
          .task_id = "task-rollback-timeout",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .occurred_at = "2026-05-24T11:00:02Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result =
              zfleet::protocol::PackageUpdateResult{
                  .component = "agent",
                  .state = "applied",
              },
      },
      std::nullopt, std::nullopt);
  REQUIRE(database.ExpireWaitingReconnect("2026-05-24T11:05:00Z").empty());
  REQUIRE(database.ExpireWaitingReconnect("2026-05-24T11:11:00Z").size() == 1);
  REQUIRE(ReadAgentField(database_path, "agent-rollback-confirm",
                         "last_upgrade_error") == "waiting_reconnect_timeout");
}
