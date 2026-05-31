#include "zfleet/platform/process.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string NormalizeNewlines(std::string value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (char ch : value) {
    if (ch != '\r') {
      normalized.push_back(ch);
    }
  }
  return normalized;
}

std::filesystem::path FixturePath() {
  return std::filesystem::path(ZFLEET_TEST_PROCESS_FIXTURE);
}

zfleet::platform::ProcessOptions MakeFixtureOptions(
    std::initializer_list<std::string> args) {
  return {
      .executable = FixturePath(),
      .args = std::vector<std::string>(args),
  };
}

}  // namespace

TEST_CASE("platform process run returns exit code") {
  const auto status =
      zfleet::platform::Run(MakeFixtureOptions({"--mode=exit", "--code=17"}));

  REQUIRE(status.exited);
  REQUIRE(status.exit_code == 17);
  REQUIRE_FALSE(status.timed_out);
}

TEST_CASE("platform process output captures stdout and stderr") {
  const auto output =
      zfleet::platform::Output(MakeFixtureOptions({"--mode=io", "--code=23"}));

  REQUIRE(output.status.exited);
  REQUIRE(output.status.exit_code == 23);
  REQUIRE(NormalizeNewlines(output.stdout_data) == "stdout-line\n");
  REQUIRE(NormalizeNewlines(output.stderr_data) == "stderr-line\n");
}

TEST_CASE("platform process can write stdin and capture output") {
  auto options = MakeFixtureOptions({"--mode=echo-stdin"});
  options.stdin_config.mode = zfleet::platform::ProcessStreamMode::kPipe;
  options.stdout_config.mode = zfleet::platform::ProcessStreamMode::kPipe;
  options.stderr_config.mode = zfleet::platform::ProcessStreamMode::kPipe;

  auto process = zfleet::platform::Process::Spawn(options);
  process.WriteStdin("payload\n");
  process.CloseStdin();

  const auto output = process.WaitWithOutput();

  REQUIRE(output.status.exited);
  REQUIRE(NormalizeNewlines(output.stdout_data) == "payload\n");
  REQUIRE(NormalizeNewlines(output.stderr_data) == "stdin-consumed\n");
}

TEST_CASE("platform process wait times out and can be terminated") {
  auto options = MakeFixtureOptions({"--mode=sleep", "--ms=500"});
  auto process = zfleet::platform::Process::Spawn(options);

  const auto status = process.Wait({.timeout = std::chrono::milliseconds(10)});
  REQUIRE(status.timed_out);
  REQUIRE_FALSE(status.exited);

  REQUIRE(process.Terminate());
  const auto finished = process.Wait();
  REQUIRE((finished.exited || !process.Valid()));
}

TEST_CASE("platform process kill ends a running process") {
  auto options = MakeFixtureOptions({"--mode=sleep", "--ms=500"});
  auto process = zfleet::platform::Process::Spawn(options);

  REQUIRE(process.Kill());
  const auto finished = process.Wait();
  REQUIRE((finished.exited || !process.Valid()));
}
