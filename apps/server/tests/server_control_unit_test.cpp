#include "control_connection_registry.h"
#include "control_dispatcher.h"
#include "control_service.h"
#include "database.h"

#include "test_util.h"
#include "http2_test_util.h"
#include "server_event_test_util.h"

#include "zfleet/transport/frame_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

using zfleet::test::ReadAgentField;

TEST_CASE("control connection registry tracks active heartbeat ownership") {
  zfleet::server::ControlConnectionRegistry registry;

  registry.OpenConnection("conn-1", "2026-05-21T10:00:00Z");
  registry.BindAgent("conn-1", "agent-1", "2026-05-21T10:00:01Z");
  registry.RecordHeartbeat("conn-1", "agent-1", "2026-05-21T10:00:05Z");

  REQUIRE(registry.ActiveConnectionCount() == 1);
  const auto first_connection = registry.FindByAgent("agent-1");
  REQUIRE(first_connection.has_value());
  REQUIRE(first_connection->connection_id == "conn-1");
  REQUIRE(first_connection->last_heartbeat_at == "2026-05-21T10:00:05Z");
  REQUIRE(registry.IsAgentOnline("agent-1", "2026-05-21T10:00:04Z"));
  REQUIRE_FALSE(registry.IsAgentOnline("agent-1", "2026-05-21T10:00:06Z"));

  registry.OpenConnection("conn-2", "2026-05-21T10:00:10Z");
  registry.BindAgent("conn-2", "agent-1", "2026-05-21T10:00:11Z");

  const auto old_connection = registry.FindByConnection("conn-1");
  const auto current_connection = registry.FindByAgent("agent-1");
  REQUIRE_FALSE(old_connection.has_value());
  REQUIRE(current_connection.has_value());
  REQUIRE(current_connection->connection_id == "conn-2");
  REQUIRE(registry.ActiveConnectionCount() == 1);

  const auto closed =
      registry.CloseConnection("conn-2", "2026-05-21T10:00:20Z");

  REQUIRE(closed.has_value());
  REQUIRE(closed->agent_id == "agent-1");
  REQUIRE(closed->was_current_agent_connection);
  REQUIRE_FALSE(registry.FindByAgent("agent-1").has_value());
  REQUIRE_FALSE(registry.FindByConnection("conn-2").has_value());
  REQUIRE(registry.ActiveConnectionCount() == 0);
}

TEST_CASE("control dispatcher decodes framed protobuf event stream") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  registry.OpenConnection("conn-1", "2026-05-21T11:00:00Z");
  zfleet::server::ControlDispatcher dispatcher(&service, &registry, "conn-1");

  const auto registration_frame = zfleet::test::EncodeEventFrame(
      zfleet::test::RegisterEvent("framed-register", "agent-1",
                                  "2026-05-21T11:00:00Z"));
  const auto heartbeat_frame = zfleet::test::EncodeEventFrame(
      zfleet::test::HeartbeatEvent("framed-heartbeat", "agent-1",
                                   "2026-05-21T11:00:05Z"));

  std::vector<std::uint8_t> stream_bytes;
  stream_bytes.insert(stream_bytes.end(), registration_frame.begin(),
                      registration_frame.end());
  stream_bytes.insert(stream_bytes.end(), heartbeat_frame.begin(),
                      heartbeat_frame.end());

  const auto partial_results = dispatcher.PushEventBytes(
      std::span<const std::uint8_t>{stream_bytes.data(), 7});
  const auto complete_results =
      dispatcher.PushEventBytes(std::span<const std::uint8_t>{
          stream_bytes.data() + 7, stream_bytes.size() - 7});

  REQUIRE(partial_results.empty());
  REQUIRE(complete_results.size() == 2);
  REQUIRE(complete_results[0].status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(complete_results[1].status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_seen_at") ==
          "2026-05-21T11:00:00Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_online_at") ==
          "2026-05-21T11:00:00Z");
  const auto connection = registry.FindByAgent("agent-1");
  REQUIRE(connection.has_value());
  REQUIRE(connection->connection_id == "conn-1");
  REQUIRE(connection->last_heartbeat_at == "2026-05-21T11:00:05Z");
  REQUIRE(registry.IsAgentOnline("agent-1", "2026-05-21T11:00:04Z"));
}

}  // namespace
