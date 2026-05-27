#include "control_server.h"
#include "control_service.h"

#include "test_util.h"

#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

TEST_CASE("control worker pool runs tasks and rejects submissions after stop") {
  zfleet::server::ControlWorkerPool worker_pool(2);

  auto result = worker_pool.Submit([]() {
    return std::vector<zfleet::server::ControlEventResult>{
        zfleet::server::ControlEventResult{
            .status = zfleet::server::ControlEventStatus::kAccepted,
            .message = "ok",
        }};
  });

  const auto events = result.get();
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(events[0].message == "ok");

  worker_pool.Stop();
  REQUIRE_THROWS_AS(worker_pool.Submit([]() {
    return std::vector<zfleet::server::ControlEventResult>{};
  }),
                    std::runtime_error);
}

TEST_CASE("control worker pool posts async completion to executor") {
  zfleet::server::ControlWorkerPool worker_pool(2);
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool completed = false;
  std::exception_ptr completion_error;
  std::vector<zfleet::server::ControlEventResult> completion_results;
  const auto accepted = worker_pool.Submit(
      []() {
        return std::vector<zfleet::server::ControlEventResult>{
            zfleet::server::ControlEventResult{
                .status = zfleet::server::ControlEventStatus::kAccepted,
                .message = "async-ok",
            }};
      },
      io_context.get_executor(),
      [&](std::exception_ptr error,
          std::vector<zfleet::server::ControlEventResult> results) {
        completion_error = error;
        completion_results = std::move(results);
        completed = true;
        work_guard.reset();
      });

  REQUIRE(accepted);
  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(completed);
  REQUIRE(completion_error == nullptr);
  REQUIRE(completion_results.size() == 1);
  REQUIRE(completion_results[0].message == "async-ok");

  worker_pool.Stop();
}

}  // namespace
