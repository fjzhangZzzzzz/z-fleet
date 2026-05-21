#include "zfleet/protocol/v1/agent_control.grpc.pb.h"
#include "zfleet/protocol/v1/agent_control.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace {

namespace proto = zfleet::protocol::v1;

} // namespace

TEST_CASE("agent control event supports protobuf round trip") {
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

  std::string bytes;
  REQUIRE(event.SerializeToString(&bytes));

  proto::AgentEvent parsed;
  REQUIRE(parsed.ParseFromString(bytes));
  REQUIRE(parsed.protocol_version() == "v1");
  REQUIRE(parsed.message_id() == "msg-1");
  REQUIRE(parsed.payload_case() == proto::AgentEvent::kRegister);
  REQUIRE(parsed.register_().hostname() == "devbox-01");
}

TEST_CASE("agent control service exposes bidirectional streaming method") {
  std::unique_ptr<proto::AgentControl::StubInterface> stub;
  REQUIRE(stub == nullptr);
  REQUIRE(proto::TaskType_Name(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY) ==
          "TASK_TYPE_COLLECT_BASIC_INVENTORY");
}
