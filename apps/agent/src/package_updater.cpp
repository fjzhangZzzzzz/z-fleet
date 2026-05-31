#include "package_updater.h"

#include "zfleet/core/component.h"
#include "zfleet/crypto/sha256.h"
#include "zfleet/package/archive.h"
#include "zfleet/platform/file_permissions.h"
#include "zfleet/platform/process.h"
#include "zfleet/transport/http_download.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zfleet::agent {
namespace {

namespace fs = std::filesystem;

fs::path DownloadPackageToFile(const std::string& url,
                               const fs::path& package_path) {
  fs::create_directories(package_path.parent_path());
  const auto status = zfleet::transport::DownloadHttpToFile(
      {.url = url, .user_agent = "zfleet_agent"}, package_path);
  if (status != 200) {
    throw std::runtime_error("package download returned status " +
                             std::to_string(status));
  }
  return package_path;
}

fs::path InstallRoot(const AgentConfig& config) {
  if (!config.install_dir.has_value()) {
    throw std::runtime_error("agent install root is unavailable");
  }
  return config.install_dir->parent_path();
}

fs::path InstallerBinary(const fs::path& root) {
  return root / "installer" / "bin" /
         zfleet::core::BinaryNameForComponent("installer");
}

int RunInstallerCommand(const fs::path& installer,
                        const std::vector<std::string>& args) {
  if (!zfleet::platform::IsLaunchableProgram(installer)) {
    throw std::runtime_error("active installer binary is unavailable");
  }
  const auto status = zfleet::platform::Run({
      .executable = installer,
      .args = args,
  });
  return status.exited ? status.exit_code : 1;
}

int RunRollback(const fs::path& installer, const fs::path& root) {
  return RunInstallerCommand(installer,
                             {"rollback", "--root", root.string(),
                              "--component", "agent"});
}

void StartNewAgent(const AgentConfig& config, const fs::path& root) {
  const auto binary =
      root / "agent" / "bin" / zfleet::core::BinaryNameForComponent("agent");
  if (!zfleet::platform::IsLaunchableProgram(binary)) {
    throw std::runtime_error("new agent binary is unavailable");
  }
  auto process = zfleet::platform::Process::Spawn({
      .executable = binary,
      .env = {"ZFLEET_COMPONENT_ROOT=" + (root / "agent").string()},
  });
  process.Detach();
  (void)config;
}

}  // namespace

PackageUpdateExecutionResult ExecutePackageUpdate(
    const AgentConfig& config,
    const zfleet::protocol::PackageUpdateInput& input) {
  try {
    if (input.action == "rollback") {
      const auto root = InstallRoot(config);
      if (RunRollback(InstallerBinary(root), root) != 0) {
        return {.error_code = zfleet::protocol::ErrorCode::apply_failed,
                .message = "installer rollback failed"};
      }
      try {
        StartNewAgent(config, root);
      } catch (const std::exception& ex) {
        return {
            .error_code = zfleet::protocol::ErrorCode::start_new_agent_failed,
            .message = ex.what()};
      }
      return {.ok = true,
              .stop_agent = true,
              .message = "package rollback applied"};
    }
    const auto package_path =
        config.data_dir / "updates" / (input.package_id + ".zip");
    DownloadPackageToFile(input.package_url, package_path);
    const auto actual_sha = zfleet::crypto::Sha256FileHex(package_path);
    if (actual_sha != input.package_sha256) {
      return {.error_code = zfleet::protocol::ErrorCode::checksum_mismatch,
              .message = "downloaded package SHA-256 mismatch"};
    }
    const auto manifest_bytes =
        zfleet::package::ReadArchiveFile(package_path, "META/manifest.json");
    const auto manifest_sha = zfleet::crypto::Sha256BytesHex(
        std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()),
                         manifest_bytes.size()));
    if (!input.manifest_sha256.empty() &&
        manifest_sha != input.manifest_sha256) {
      return {.error_code = zfleet::protocol::ErrorCode::checksum_mismatch,
              .message = "downloaded manifest SHA-256 mismatch"};
    }

    const auto root = InstallRoot(config);
    if (RunInstallerCommand(InstallerBinary(root),
                            {"apply", "--root", root.string(), "--package",
                             package_path.string()}) != 0) {
      return {.error_code = zfleet::protocol::ErrorCode::apply_failed,
              .message = "installer apply failed"};
    }
    if (input.component == "agent") {
      try {
        StartNewAgent(config, root);
      } catch (const std::exception& ex) {
        return {
            .error_code = zfleet::protocol::ErrorCode::start_new_agent_failed,
            .message = ex.what()};
      }
    }
    return {.ok = true,
            .stop_agent = input.component == "agent",
            .message = "package update applied"};
  } catch (const std::exception& ex) {
    return {.error_code = zfleet::protocol::ErrorCode::download_failed,
            .message = ex.what()};
  }
}

}  // namespace zfleet::agent
