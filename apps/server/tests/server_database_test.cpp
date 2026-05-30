#include "config.h"
#include "database.h"
#include "package_repository.h"

#include "test_util.h"

#include "zfleet/crypto/sha256.h"
#include "zfleet/package/archive.h"
#include "zfleet/package/manifest.h"
#include "zfleet/protocol/v1/agent_control.pb.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace proto = zfleet::protocol::v1;

bool TableExists(const std::filesystem::path& database_path,
                 const std::string& table_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select name from sqlite_master where type = 'table' and name = ?");
  query.bind(1, table_name);
  return query.executeStep();
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
  payload->set_hostname("devbox-web");
  payload->set_os("linux");
  payload->set_os_version("6.8");
  payload->set_arch("x86_64");
  payload->set_agent_version("0.1.0");
  payload->add_applications("cmake");
  payload->add_applications("ninja");
  payload->add_services("zfleet-agent");
  return event;
}

void SeedAgent(zfleet::server::ServerDatabase* database,
               const std::string& agent_id) {
  database->UpsertAgent(zfleet::protocol::AgentRegistration{
      .protocol_version = "v1",
      .request_id = "seed-agent",
      .agent_id = agent_id,
      .occurred_at = "2026-05-24T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-web",
      .os = "linux",
      .arch = "x86_64",
  });
}

}  // namespace

TEST_CASE("server database initializes schema and version") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();

  REQUIRE(fs::exists(database_path));
  REQUIRE(database.schema_version() == 10);
  REQUIRE(TableExists(database_path, "agents"));
  REQUIRE_FALSE(TableExists(database_path, "heartbeats"));
  REQUIRE(TableExists(database_path, "asset_snapshots"));
  REQUIRE(TableExists(database_path, "audit_events"));
  REQUIRE(TableExists(database_path, "tasks"));
  REQUIRE(TableExists(database_path, "task_results"));
  REQUIRE(TableExists(database_path, "agent_packages"));
  REQUIRE(TableExists(database_path, "package_publications"));
  REQUIRE(TableExists(database_path, "registration_tokens"));
}

TEST_CASE("server database exposes web admin read models") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-web-1");
  database.RecordAssetSnapshot(
      zfleet::protocol::AssetSnapshot{
          .protocol_version = "v1",
          .request_id = "asset-web-1",
          .agent_id = "agent-web-1",
          .occurred_at = "2026-05-24T10:00:00Z",
          .hostname = "devbox-web",
          .os = "linux",
          .os_version = "6.8",
          .arch = "x86_64",
          .agent_version = "0.1.0",
          .applications = {"cmake", "ninja"},
          .services = {"zfleet-agent"},
      },
      AssetSnapshotEvent("asset-web-1", "agent-web-1", "2026-05-24T10:00:00Z"));

  const auto agents = database.ListAgents();
  REQUIRE(agents.size() == 1);
  REQUIRE(agents.front().agent_id == "agent-web-1");
  REQUIRE(agents.front().status == "online");

  const auto latest = database.FindLatestAssetSnapshot("agent-web-1");
  REQUIRE(latest.has_value());
  REQUIRE(latest->hostname == "devbox-web");
  REQUIRE(latest->os_version == "6.8");
  REQUIRE((latest->applications == std::vector<std::string>{"cmake", "ninja"}));
  REQUIRE((latest->services == std::vector<std::string>{"zfleet-agent"}));
}

TEST_CASE("server database stores package channel and registration tokens") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();

  database.UpsertAgentPackage(zfleet::server::AgentPackageRecord{
      .package_id = "pkg-1",
      .component = "agent",
      .version = "0.1.0",
      .platform = "linux",
      .arch = "x86_64",
      .build_type = "release",
      .filename = "zfleet_agent-v0.1.0-linux-x86_64-release.zip",
      .storage_path = test_root / "agent.zip",
      .size_bytes = 10,
      .sha256 = std::string(64, 'a'),
      .manifest_json = "{}",
      .status = "validated",
      .uploaded_at = "2026-05-24T10:00:00Z",
      .validated_at = "2026-05-24T10:00:00Z",
  });
  database.PublishAgentPackage("pkg-1", "agent", "stable", "linux", "x86_64",
                               "release", std::optional<std::string>{"admin"},
                               "2026-05-24T10:01:00Z");

  const auto package = database.FindDefaultPublishedAgentPackage(
      "agent", "stable", "linux", "x86_64", "release");
  REQUIRE(package.has_value());
  REQUIRE(package->package_id == "pkg-1");
  REQUIRE(package->status == "published");
  const auto stored_package = database.FindAgentPackage("pkg-1");
  REQUIRE(stored_package.has_value());
  REQUIRE(stored_package->published_channels ==
          std::vector<std::string>{"stable"});

  database.RetireAgentPackage("pkg-1", "2026-05-24T10:02:00Z");
  const auto retired_package = database.FindAgentPackage("pkg-1");
  REQUIRE(retired_package.has_value());
  REQUIRE(retired_package->status == "retired");
  REQUIRE(retired_package->retired_at == "2026-05-24T10:02:00Z");
  REQUIRE(retired_package->published_channels.empty());

  database.CreateRegistrationToken(zfleet::server::RegistrationTokenRecord{
      .token_id = "token-1",
      .token_hash = std::string(64, 'b'),
      .purpose = "agent_register",
      .channel = std::optional<std::string>{"stable"},
      .platform = std::optional<std::string>{"linux"},
      .arch = std::optional<std::string>{"x86_64"},
      .max_uses = 1,
      .use_count = 0,
      .status = "active",
      .created_at = "2026-05-24T10:02:00Z",
      .expires_at = "2026-05-25T10:02:00Z",
  });
  const auto tokens = database.ListRegistrationTokens();
  REQUIRE(tokens.size() == 1);
  REQUIRE(tokens.front().token_id == "token-1");
  REQUIRE(tokens.front().token_hash == std::string(64, 'b'));
  REQUIRE(tokens.front().status == "active");

  REQUIRE(database.RevokeRegistrationToken("token-1", "2026-05-24T10:03:00Z"));
  const auto revoked_tokens = database.ListRegistrationTokens();
  REQUIRE(revoked_tokens.front().status == "revoked");
  REQUIRE(revoked_tokens.front().revoked_at ==
          std::optional<std::string>{"2026-05-24T10:03:00Z"});
}

TEST_CASE("server package validation checks archive metadata and filename") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto package_dir = test_root / "package";
  zfleet::test::WriteTextFile(package_dir / "payload" / "bin" / "zfleet_agent",
                              "agent-binary");
  const auto manifest =
      zfleet::package::SerializeManifestJson(zfleet::package::Manifest{
          .schema_version = 2,
          .component = "agent",
          .version = "0.1.0",
          .platform = "linux",
          .arch = "x86_64",
          .build_type = "release",
          .min_installer_version = "0.1.0",
          .files = {zfleet::package::ManifestFile{
              .source = "payload/bin/zfleet_agent",
              .target = "bin/zfleet_agent",
              .size = 12,
              .sha256 = zfleet::crypto::Sha256BytesHex("agent-binary"),
              .launchable = true,
          }},
      });
  zfleet::test::WriteTextFile(package_dir / "META" / "manifest.json", manifest);
  const auto archive_path =
      test_root / "zfleet_agent-v0.1.0-linux-x86_64-release.zip";
  zfleet::package::CreateArchive(zfleet::package::CreateArchiveOptions{
      .package_dir = package_dir,
      .archive_path = archive_path,
      .force = true,
  });

  const auto metadata = zfleet::server::ValidateAgentPackageUpload(
      archive_path, "zfleet_agent-v0.1.0-linux-x86_64-release.zip");
  REQUIRE(metadata.component == "agent");
  REQUIRE(metadata.version == "0.1.0");
  REQUIRE(metadata.platform == "linux");
  REQUIRE(metadata.arch == "x86_64");
  REQUIRE(metadata.build_type == "release");
}

