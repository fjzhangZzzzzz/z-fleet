#include "zfleet/protocol/v1/agent_control.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

namespace proto = zfleet::protocol::v1;

} // namespace

TEST_CASE("agent control event supports protobuf-lite round trip") {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id("msg-1");
  event.set_correlation_id("corr-1");
  event.set_agent_id("agent-1");
  event.set_occurred_at("2026-05-21T10:00:00Z");

  auto* registration = event.mutable_register_();
  registration->set_agent_version("0.1.0");
  registration->set_hostname("devbox-01");
  registration->set_os("linux");
  registration->set_arch("x86_64");
  registration->set_registration_token("token-once");

  std::string bytes;
  REQUIRE(event.SerializeToString(&bytes));

  proto::AgentEvent parsed;
  REQUIRE(parsed.ParseFromString(bytes));
  REQUIRE(parsed.protocol_version() == "v1");
  REQUIRE(parsed.message_id() == "msg-1");
  REQUIRE(parsed.payload_case() == proto::AgentEvent::kRegister);
  REQUIRE(parsed.register_().hostname() == "devbox-01");
  REQUIRE(parsed.register_().registration_token() == "token-once");
}

TEST_CASE("package update task contract carries verified package metadata") {
  proto::ServerCommand command;
  auto* task = command.mutable_task_assigned();
  task->set_task_id("upgrade-1");
  task->set_task_type(proto::TASK_TYPE_PACKAGE_UPDATE);
  task->set_capability_level(proto::CAPABILITY_LEVEL_HIGH_RISK_WRITE);
  auto* input = task->mutable_package_update();
  input->set_action("apply");
  input->set_component("agent");
  input->set_package_id("pkg-agent-1");
  input->set_version("0.2.0");
  input->set_platform("linux");
  input->set_arch("x86_64");
  input->set_build_type("release");
  input->set_package_sha256(std::string(64, 'a'));

  std::string bytes;
  REQUIRE(command.SerializeToString(&bytes));
  proto::ServerCommand parsed;
  REQUIRE(parsed.ParseFromString(bytes));
  REQUIRE(parsed.task_assigned().task_type() == proto::TASK_TYPE_PACKAGE_UPDATE);
  REQUIRE(parsed.task_assigned().package_update().package_id() ==
          "pkg-agent-1");
  REQUIRE(parsed.task_assigned().package_update().action() == "apply");
  REQUIRE(parsed.task_assigned().capability_level() ==
          proto::CAPABILITY_LEVEL_HIGH_RISK_WRITE);
}

TEST_CASE("server command supports protobuf-lite enum names") {
  proto::ServerCommand command;
  command.set_protocol_version("v1");
  command.set_message_id("cmd-1");
  command.set_correlation_id("msg-1");
  command.set_agent_id("agent-1");
  command.set_occurred_at("2026-05-21T10:00:01Z");

  auto* task = command.mutable_task_assigned();
  task->set_task_id("task-1");
  task->set_task_type(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  task->set_capability_level(proto::CAPABILITY_LEVEL_READONLY);

  std::string bytes;
  REQUIRE(command.SerializeToString(&bytes));

  proto::ServerCommand parsed;
  REQUIRE(parsed.ParseFromString(bytes));
  REQUIRE(parsed.payload_case() == proto::ServerCommand::kTaskAssigned);
  REQUIRE(parsed.task_assigned().task_type() ==
          proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  REQUIRE(proto::TaskType_Name(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY) ==
          "TASK_TYPE_COLLECT_BASIC_INVENTORY");
}
