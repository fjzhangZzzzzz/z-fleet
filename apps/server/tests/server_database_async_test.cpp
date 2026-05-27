#include "database.h"

#include "test_util.h"

#include "zfleet/protocol/v1/agent_control.pb.h"

#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <filesystem>
#include <string>
#include <thread>

namespace {

TEST_CASE(
    "server database async submission reports stopped actor on executor") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.Stop();
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool completed = false;
  std::exception_ptr completion_error;
  database.AsyncEnqueueTask(
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-async-after-stop",
          .agent_id = "agent-async-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .capability_level = zfleet::protocol::CapabilityLevel::readonly,
          .created_at = "2026-05-21T10:00:00Z",
          .expires_at = "2099-05-21T10:05:00Z",
          .input = zfleet::protocol::CollectBasicInventoryInput{},
      },
      io_context.get_executor(), [&](std::exception_ptr error) {
        completion_error = error;
        completed = true;
        work_guard.reset();
      });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(completed);
  REQUIRE(completion_error != nullptr);
}

}  // namespace
