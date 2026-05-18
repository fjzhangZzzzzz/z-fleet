#include "launcher.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

std::atomic<int> g_test_sequence = 0;

int CurrentProcessId() {
#ifdef _WIN32
  return _getpid();
#else
  return getpid();
#endif
}

fs::path MakeTestRoot() {
  return fs::temp_directory_path() / "zfleet-launcher-tests" /
         (std::to_string(CurrentProcessId()) + "-" +
          std::to_string(g_test_sequence.fetch_add(1)));
}

void WriteTextFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  REQUIRE(stream);
  stream << content;
  REQUIRE(stream.good());
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

void SetExecutable(const fs::path& path) {
  const auto mask = fs::perms::owner_exec | fs::perms::group_exec |
                    fs::perms::others_exec;
  fs::permissions(path, mask, fs::perm_options::add);
}

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
  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);

  const auto launcher_path =
      test_root / "zfleet" / "agent" / "bin" / "zfleet_agent";
  WriteTextFile(test_root / "zfleet" / "agent" / "var" / "active-version",
                "1.2.3\n");
  WriteTextFile(test_root / "zfleet" / "agent" / "releases" / "1.2.3" / "bin" /
                    "zfleet_agent",
                "agent-binary");
  SetExecutable(test_root / "zfleet" / "agent" / "releases" / "1.2.3" / "bin" /
                "zfleet_agent");

  const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);

  REQUIRE(resolved.ok);
  REQUIRE(resolved.target.component == "agent");
  REQUIRE(resolved.target.component_root == test_root / "zfleet" / "agent");
  REQUIRE(resolved.target.version == "1.2.3");
  REQUIRE(resolved.target.executable_path ==
          test_root / "zfleet" / "agent" / "releases" / "1.2.3" / "bin" /
              "zfleet_agent");

  fs::remove_all(test_root);
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
  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);

  const auto launcher_path =
      test_root / "zfleet" / "agent" / "bin" / "zfleet_agent";

  SECTION("missing active-version") {
    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message == "active-version is missing");
  }

  SECTION("invalid active-version") {
    WriteTextFile(test_root / "zfleet" / "agent" / "var" / "active-version",
                  "../bad\n");

    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message == "active-version is invalid");
  }

  SECTION("target executable missing") {
    WriteTextFile(test_root / "zfleet" / "agent" / "var" / "active-version",
                  "1.2.3\n");

    const auto resolved = zfleet::launcher::ResolveLaunchTarget(launcher_path);
    REQUIRE_FALSE(resolved.ok);
    REQUIRE(resolved.message == "target executable is missing or not executable");
  }

  fs::remove_all(test_root);
}

#ifndef _WIN32
TEST_CASE("launcher executable forwards args and propagates exit code on POSIX") {
  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);

  const fs::path launcher_source = ZFLEET_TEST_AGENT_LAUNCHER;
  const auto launcher_path =
      test_root / "zfleet" / "agent" / "bin" / "zfleet_agent";
  fs::create_directories(launcher_path.parent_path());
  REQUIRE(fs::copy_file(launcher_source, launcher_path,
                        fs::copy_options::overwrite_existing));
  fs::permissions(launcher_path, fs::status(launcher_source).permissions());

  WriteTextFile(test_root / "zfleet" / "agent" / "var" / "active-version",
                "2.0.0\n");
  const auto args_file = test_root / "args.txt";
  const auto target_path =
      test_root / "zfleet" / "agent" / "releases" / "2.0.0" / "bin" /
      "zfleet_agent";
  WriteTextFile(target_path,
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > \"" + args_file.string() + "\"\n"
                "exit 23\n");
  SetExecutable(target_path);

  const auto exit_code =
      RunProcess(launcher_path, {"alpha", "beta gamma", "--flag"});

  REQUIRE(exit_code == 23);
  REQUIRE(ReadTextFile(args_file) == "alpha\nbeta gamma\n--flag\n");

  fs::remove_all(test_root);
}
#endif
