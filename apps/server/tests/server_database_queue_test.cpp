#include "database.h"

#include "test_util.h"
#include "server_event_test_util.h"
#include "sqlite_test_util.h"

#include "zfleet/protocol/v1/agent_control.pb.h"

#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <thread>

namespace {

using zfleet::test::ReadTaskField;
using zfleet::test::SeedTask;

TEST_CASE("server database claims queued task only once") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedTask(&database, "task-claim-1", "agent-1");

  const auto first_claim =
      database.ClaimNextTaskForAgent("agent-1", "2026-05-21T10:00:01Z");
  const auto second_claim =
      database.ClaimNextTaskForAgent("agent-1", "2026-05-21T10:00:02Z");

  REQUIRE(first_claim.has_value());
  REQUIRE(first_claim->task_id == "task-claim-1");
  REQUIRE_FALSE(second_claim.has_value());
  REQUIRE(ReadTaskField(database_path, "task-claim-1", "state") == "assigned");
}

TEST_CASE(
    "server database serializes concurrent task claims through write actor") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedTask(&database, "task-concurrent-claim-1", "agent-1");

  constexpr int kClaimers = 16;
  std::atomic_bool start{false};
  std::vector<std::future<std::optional<zfleet::protocol::Task>>> claims;
  claims.reserve(kClaimers);
  for (int i = 0; i < kClaimers; ++i) {
    claims.push_back(std::async(std::launch::async, [&database, &start, i]() {
      while (!start.load()) {
        std::this_thread::yield();
      }
      return database.ClaimNextTaskForAgent(
          "agent-1", "2026-05-21T10:00:" + std::to_string(10 + i) + "Z");
    }));
  }

  start.store(true);
  int claimed_count = 0;
  for (auto& claim : claims) {
    const auto task = claim.get();
    if (task.has_value()) {
      ++claimed_count;
      REQUIRE(task->task_id == "task-concurrent-claim-1");
    }
  }

  REQUIRE(claimed_count == 1);
  REQUIRE(ReadTaskField(database_path, "task-concurrent-claim-1", "state") ==
          "assigned");
}

TEST_CASE(
    "server database stop closes write actor and wakes task queue waiters") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const auto version = database.TaskQueueVersion();

  auto waiter = std::async(std::launch::async, [&database, version]() {
    return database.WaitForTaskQueueChange(version, std::chrono::seconds(30));
  });

  database.Stop();

  REQUIRE(waiter.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
  REQUIRE(waiter.get() == version);
  REQUIRE_THROWS_AS(
      database.EnqueueTask(zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-after-stop",
          .agent_id = "agent-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .capability_level = zfleet::protocol::CapabilityLevel::readonly,
          .created_at = "2026-05-21T10:00:00Z",
          .expires_at = "2099-05-21T10:05:00Z",
          .input = zfleet::protocol::CollectBasicInventoryInput{},
      }),
      std::runtime_error);
}

TEST_CASE("server database async operations post completions to executor") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool enqueue_completed = false;
  std::exception_ptr enqueue_error;
  database.AsyncEnqueueTask(
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-async-1",
          .agent_id = "agent-async-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .capability_level = zfleet::protocol::CapabilityLevel::readonly,
          .created_at = "2026-05-21T10:00:00Z",
          .expires_at = "2099-05-21T10:05:00Z",
          .input = zfleet::protocol::CollectBasicInventoryInput{},
      },
      io_context.get_executor(), [&](std::exception_ptr error) {
        enqueue_error = error;
        enqueue_completed = true;
        work_guard.reset();
      });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(enqueue_completed);
  REQUIRE(enqueue_error == nullptr);
  REQUIRE(ReadTaskField(database_path, "task-async-1", "state") == "queued");
}

TEST_CASE("server database task queue subscriptions observe enqueue changes") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool notified = false;
  std::uint64_t observed_version = 0;
  const auto subscription = database.SubscribeTaskQueueChanges(
      io_context.get_executor(), [&](std::uint64_t version) {
        observed_version = version;
        notified = true;
        work_guard.reset();
      });

  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-subscription-1",
      .agent_id = "agent-subscription-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-21T10:00:00Z",
      .expires_at = "2099-05-21T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  database.UnsubscribeTaskQueueChanges(subscription);

  REQUIRE(notified);
  REQUIRE(observed_version > 0);
  REQUIRE(ReadTaskField(database_path, "task-subscription-1", "state") ==
          "queued");
}

TEST_CASE("server database async claim keeps single assignment semantics") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedTask(&database, "task-async-claim-1", "agent-async-claim-1");
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  int completion_count = 0;
  int claimed_count = 0;
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  database.AsyncClaimNextTaskForAgent(
      "agent-async-claim-1", "2026-05-21T10:00:01Z", io_context.get_executor(),
      [&](std::exception_ptr error,
          std::optional<zfleet::protocol::Task> task) {
        first_error = error;
        if (task.has_value()) {
          ++claimed_count;
        }
        ++completion_count;
        if (completion_count == 2) {
          work_guard.reset();
        }
      });
  database.AsyncClaimNextTaskForAgent(
      "agent-async-claim-1", "2026-05-21T10:00:02Z", io_context.get_executor(),
      [&](std::exception_ptr error,
          std::optional<zfleet::protocol::Task> task) {
        second_error = error;
        if (task.has_value()) {
          ++claimed_count;
        }
        ++completion_count;
        if (completion_count == 2) {
          work_guard.reset();
        }
      });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(completion_count == 2);
  REQUIRE(first_error == nullptr);
  REQUIRE(second_error == nullptr);
  REQUIRE(claimed_count == 1);
  REQUIRE(ReadTaskField(database_path, "task-async-claim-1", "state") ==
          "assigned");
}

TEST_CASE("server database rejects mismatched task type and input payload") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();

  const zfleet::protocol::Task mismatched{
      .protocol_version = "v1",
      .task_id = "task-mismatched-input",
      .agent_id = "agent-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-25T10:00:00Z",
      .expires_at = "2099-05-25T10:10:00Z",
      .input =
          zfleet::protocol::PackageUpdateInput{
              .action = "apply",
              .component = "agent",
          },
  };

  REQUIRE_THROWS(database.EnqueueTask(mismatched));
}

}  // namespace
