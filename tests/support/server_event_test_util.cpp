#include "server_event_test_util.h"

#include "database.h"

#include "zfleet/protocol/control_codec.h"
#include "zfleet/protocol/v1/agent_control.pb.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <span>
#include <stdexcept>

namespace zfleet::test {

namespace proto = zfleet::protocol::v1;

std::string ReadAgentField(const std::filesystem::path& database_path,
                           const std::string& agent_id,
                           const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from agents where agent_id = ?");
  query.bind(1, agent_id);
  if (!query.executeStep() || query.getColumn(0).isNull()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadAuditField(const std::filesystem::path& database_path,
                           const std::string& request_id,
                           const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(db, "select " + column_name +
                                  " from audit_events where request_id = ? "
                                  "order by rowid desc limit 1");
  query.bind(1, request_id);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadTaskField(const std::filesystem::path& database_path,
                          const std::string& task_id,
                          const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from tasks where task_id = ?");
  query.bind(1, task_id);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadTaskResultField(const std::filesystem::path& database_path,
                                const std::string& task_id,
                                const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from task_results where task_id = ?");
  query.bind(1, task_id);
  if (!query.executeStep() || query.getColumn(0).isNull()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadTaskResultBlob(const std::filesystem::path& database_path,
                               const std::string& task_id,
                               const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from task_results where task_id = ?");
  query.bind(1, task_id);
  if (!query.executeStep() || query.getColumn(0).isNull()) {
    return {};
  }
  const auto column = query.getColumn(0);
  return std::string(static_cast<const char*>(column.getBlob()),
                     static_cast<std::size_t>(column.getBytes()));
}

proto::AgentEvent RegisterEvent(std::string message_id, std::string agent_id,
                                std::string occurred_at,
                                std::string registration_token,
                                std::string agent_version) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_register_();
  payload->set_agent_version(std::move(agent_version));
  payload->set_hostname("devbox-01");
  payload->set_os("linux");
  payload->set_arch("x86_64");
  payload->set_registration_token(std::move(registration_token));
  return event;
}

proto::AgentEvent HeartbeatEvent(std::string message_id, std::string agent_id,
                                 std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  event.mutable_heartbeat()->set_agent_version("0.1.0");
  return event;
}

proto::AgentEvent AssetSnapshotEvent(std::string message_id,
                                     std::string agent_id,
                                     std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_asset_snapshot();
  payload->set_hostname("devbox-01");
  payload->set_os("linux");
  payload->set_os_version("6.8");
  payload->set_arch("x86_64");
  payload->set_agent_version("0.1.0");
  payload->add_applications("cmake");
  payload->add_services("zfleet-agent");
  return event;
}

proto::AgentEvent TaskRunningEvent(std::string message_id, std::string agent_id,
                                   std::string task_id,
                                   std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_task_running();
  payload->set_task_id(std::move(task_id));
  payload->set_task_type(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  return event;
}

proto::AgentEvent TaskSucceededEvent(std::string message_id,
                                     std::string agent_id, std::string task_id,
                                     std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_task_result();
  payload->set_task_id(std::move(task_id));
  payload->set_task_type(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  payload->set_status(proto::TASK_EXECUTION_STATUS_SUCCEEDED);
  auto* inventory = payload->mutable_collect_basic_inventory();
  inventory->set_hostname("devbox-01");
  inventory->set_os("linux");
  inventory->set_arch("x86_64");
  inventory->set_agent_version("0.1.0");
  return event;
}

proto::AgentEvent TaskFailedEvent(std::string message_id, std::string agent_id,
                                  std::string task_id,
                                  std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_task_result();
  payload->set_task_id(std::move(task_id));
  payload->set_task_type(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  payload->set_status(proto::TASK_EXECUTION_STATUS_FAILED);
  auto* error = payload->mutable_error();
  error->set_code(proto::ERROR_CODE_INTERNAL_ERROR);
  error->set_message("inventory failed");
  error->set_retryable(true);
  return event;
}

zfleet::protocol::AgentEvent DomainAgentEvent(const proto::AgentEvent& event) {
  std::string bytes;
  if (!event.SerializeToString(&bytes)) {
    throw std::runtime_error("failed to serialize test agent event");
  }
  return zfleet::protocol::DecodeAgentEventPayload(
      std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

void SeedAgent(zfleet::server::ServerDatabase* database,
               const std::string& agent_id) {
  database->UpsertAgent(zfleet::protocol::AgentRegistration{
      .protocol_version = "v1",
      .request_id = "seed-agent",
      .agent_id = agent_id,
      .occurred_at = "2026-05-21T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
}

void SeedTask(zfleet::server::ServerDatabase* database,
              const std::string& task_id, const std::string& agent_id) {
  database->EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = task_id,
      .agent_id = agent_id,
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-21T10:00:00Z",
      .expires_at = "2099-05-21T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });
}

}  // namespace zfleet::test
