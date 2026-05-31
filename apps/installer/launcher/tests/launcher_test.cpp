#include "launcher.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <zfleet/core/component.h>
#include <zfleet/platform/file_permissions.h>
#include <zfleet/platform/process.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

}  // namespace

TEST_CASE("resolve target derives component root from launcher path") {
  const zfleet::test::ScopedTestDir test_dir("launcher");
  const auto test_root = test_dir.path();

  const auto launcher_binary = zfleet::core::BinaryNameForComponent("agent");
  const auto launcher_path = test_root / "agent" / "bin" / launcher_binary;
  zfleet::test::WriteTextFile(test_root / "agent" / "var" / "active-version",
                              "1.2.3\n");
  zfleet::test::WriteTextFile(
      test_root / "agent" / "releases" / "1.2.3" / "bin" / launcher_binary,
      "agent-binary");
  zfleet::platform::SetExecutable(
      test_root / "agent" / "releases" / "1.2.3" / "bin" / launcher_binary,
      true);

  const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);

  REQUIRE(resolved.ok);
  REQUIRE(resolved.target.component == "agent");
  REQUIRE(resolved.target.component_root == test_root / "agent");
  REQUIRE(resolved.target.version == "1.2.3");
  REQUIRE(resolved.target.executable_path ==
          test_root / "agent" / "releases" / "1.2.3" / "bin" / launcher_binary);
}

TEST_CASE("forwarded argv preserves argv[1..] and replaces argv[0]") {
  char arg0[] = "stub";
  char arg1[] = "--mode";
  char arg2[] = "beta";
  char* argv[] = {arg0, arg1, arg2};

  const auto forwarded = zfleet::launcher::BuildForwardedArgv(
      "/tmp/example/agent/releases/1.2.3/bin/zfleet_agent", 3, argv);

  REQUIRE(forwarded.size() == 3);
  REQUIRE(forwarded[0] == "/tmp/example/agent/releases/1.2.3/bin/zfleet_agent");
  REQUIRE(forwarded[1] == "--mode");
  REQUIRE(forwarded[2] == "beta");
}

TEST_CASE("resolve target fails for invalid active version state") {
  const zfleet::test::ScopedTestDir test_dir("launcher");
  const auto test_root = test_dir.path();

  const auto launcher_path =
      test_root / "agent" / "bin" / zfleet::core::BinaryNameForComponent("agent");

  SECTION("missing active-version") {
    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message == "active-version is missing");
  }

  SECTION("invalid active-version") {
    zfleet::test::WriteTextFile(test_root / "agent" / "var" / "active-version",
                                "../bad\n");

    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(
        resolved.message ==
        "active-version is invalid: expected non-empty single segment using "
        "only letters, digits, '.', '_', or '-'");
  }

  SECTION("target executable missing") {
    zfleet::test::WriteTextFile(test_root / "agent" / "var" / "active-version",
                                "1.2.3\n");

    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message ==
            "target executable is missing or not executable");
  }
}

TEST_CASE(
    "launcher executable forwards args and propagates exit code on POSIX") {
  const zfleet::test::ScopedTestDir test_dir("launcher");
  const auto test_root = test_dir.path();

  const fs::path launcher_source = ZFLEET_TEST_AGENT_LAUNCHER;
  const auto launcher_path =
      test_root / "agent" / "bin" / zfleet::core::BinaryNameForComponent("agent");
  fs::create_directories(launcher_path.parent_path());
  REQUIRE(fs::copy_file(launcher_source, launcher_path,
                        fs::copy_options::overwrite_existing));
  fs::permissions(launcher_path, fs::status(launcher_source).permissions());

  zfleet::test::WriteTextFile(test_root / "agent" / "var" / "active-version",
                              "2.0.0\n");
  const auto args_file = test_root / "args.txt";
  const auto env_file = test_root / "env.txt";
  const auto target_path =
      test_root / "agent" / "releases" / "2.0.0" / "bin" /
      zfleet::core::BinaryNameForComponent("agent");
  zfleet::test::WriteTextFile(
      target_path,
      "#!/bin/sh\n"
      "printf '%s\\n' \"$@\" > \"" +
          args_file.string() +
          "\"\n"
          "printf '%s\\n' \"$ZFLEET_COMPONENT_ROOT\" > \"" +
          env_file.string() +
          "\"\n"
          "exit 23\n");
  zfleet::platform::SetExecutable(target_path, true);

  const auto status = zfleet::platform::Run({
      .executable = launcher_path,
      .args = {"alpha", "beta gamma", "--flag"},
  });

  REQUIRE(status.exited);
  REQUIRE(status.exit_code == 23);
  REQUIRE(zfleet::test::ReadTextFile(args_file) ==
          "alpha\nbeta gamma\n--flag\n");
  REQUIRE(zfleet::test::ReadTextFile(env_file) ==
          (test_root / "agent").string() + "\n");
}
