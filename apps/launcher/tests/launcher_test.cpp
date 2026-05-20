#include "launcher.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <zfleet/platform/file_permissions.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

#ifndef _WIN32
int RunProcess(const fs::path& executable,
               const std::vector<std::string>& args) {
  const auto pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    std::vector<std::string> argv_strings;
    argv_strings.reserve(args.size() + 1);
    argv_strings.push_back(executable.string());
    argv_strings.insert(argv_strings.end(), args.begin(), args.end());

    std::vector<char*> raw_args;
    raw_args.reserve(argv_strings.size() + 1);
    for (auto& value : argv_strings) {
      raw_args.push_back(value.data());
    }
    raw_args.push_back(nullptr);

    execv(executable.c_str(), raw_args.data());
    _exit(127);
  }

  int status = 0;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  return WEXITSTATUS(status);
}
#endif

} // namespace

TEST_CASE("resolve target derives component root from launcher path") {
  const zfleet::test::ScopedTestDir test_dir("launcher");
  const auto test_root = test_dir.path();

  const auto launcher_path =
      test_root / "zfleet" / "agent" / "bin" / "zfleet_agent";
  zfleet::test::WriteTextFile(test_root / "zfleet" / "agent" / "var" /
                                  "active-version",
                              "1.2.3\n");
  zfleet::test::WriteTextFile(test_root / "zfleet" / "agent" / "releases" /
                                  "1.2.3" / "bin" / "zfleet_agent",
                              "agent-binary");
  zfleet::platform::SetExecutable(test_root / "zfleet" / "agent" /
                                      "releases" / "1.2.3" / "bin" /
                                      "zfleet_agent",
                                  true);

  const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);

  REQUIRE(resolved.ok);
  REQUIRE(resolved.target.component == "agent");
  REQUIRE(resolved.target.component_root == test_root / "zfleet" / "agent");
  REQUIRE(resolved.target.version == "1.2.3");
  REQUIRE(resolved.target.executable_path ==
          test_root / "zfleet" / "agent" / "releases" / "1.2.3" / "bin" /
              "zfleet_agent");
}

TEST_CASE("forwarded argv preserves argv[1..] and replaces argv[0]") {
  char arg0[] = "stub";
  char arg1[] = "--mode";
  char arg2[] = "beta";
  char* argv[] = {arg0, arg1, arg2};

  const auto forwarded = zfleet::launcher::BuildForwardedArgv(
      "/tmp/zfleet/agent/releases/1.2.3/bin/zfleet_agent", 3, argv);

  REQUIRE(forwarded.size() == 3);
  REQUIRE(forwarded[0] ==
          "/tmp/zfleet/agent/releases/1.2.3/bin/zfleet_agent");
  REQUIRE(forwarded[1] == "--mode");
  REQUIRE(forwarded[2] == "beta");
}

TEST_CASE("resolve target fails for invalid active version state") {
  const zfleet::test::ScopedTestDir test_dir("launcher");
  const auto test_root = test_dir.path();

  const auto launcher_path =
      test_root / "zfleet" / "agent" / "bin" / "zfleet_agent";

  SECTION("missing active-version") {
    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message == "active-version is missing");
  }

  SECTION("invalid active-version") {
    zfleet::test::WriteTextFile(
        test_root / "zfleet" / "agent" / "var" / "active-version",
        "../bad\n");

    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message ==
            "active-version is invalid: expected non-empty single segment using "
            "only letters, digits, '.', '_', or '-'");
  }

  SECTION("target executable missing") {
    zfleet::test::WriteTextFile(
        test_root / "zfleet" / "agent" / "var" / "active-version",
        "1.2.3\n");

    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message == "target executable is missing or not executable");
  }

}

#ifndef _WIN32
TEST_CASE("launcher executable forwards args and propagates exit code on POSIX") {
  const zfleet::test::ScopedTestDir test_dir("launcher");
  const auto test_root = test_dir.path();

  const fs::path launcher_source = ZFLEET_TEST_AGENT_LAUNCHER;
  const auto launcher_path =
      test_root / "zfleet" / "agent" / "bin" / "zfleet_agent";
  fs::create_directories(launcher_path.parent_path());
  REQUIRE(fs::copy_file(launcher_source, launcher_path,
                        fs::copy_options::overwrite_existing));
  fs::permissions(launcher_path, fs::status(launcher_source).permissions());

  zfleet::test::WriteTextFile(test_root / "zfleet" / "agent" / "var" /
                                  "active-version",
                              "2.0.0\n");
  const auto args_file = test_root / "args.txt";
  const auto target_path =
      test_root / "zfleet" / "agent" / "releases" / "2.0.0" / "bin" /
      "zfleet_agent";
  zfleet::test::WriteTextFile(target_path,
                              "#!/bin/sh\n"
                              "printf '%s\\n' \"$@\" > \"" +
                                  args_file.string() + "\"\n"
                              "exit 23\n");
  zfleet::platform::SetExecutable(target_path, true);

  const auto exit_code =
      RunProcess(launcher_path, {"alpha", "beta gamma", "--flag"});

  REQUIRE(exit_code == 23);
  REQUIRE(zfleet::test::ReadTextFile(args_file) == "alpha\nbeta gamma\n--flag\n");
}
#endif
