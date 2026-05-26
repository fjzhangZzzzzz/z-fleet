#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zfleet::server {

struct InstallCommandRequest {
  std::string server_url;
  std::string control_url;
  std::string token;
  std::string channel;
  std::string root;
};

struct InstallPlatformSpec {
  std::string platform;
  std::string script_path;
  std::string script_content_type;
  std::string (*build_command)(const InstallCommandRequest& request);
};

const InstallPlatformSpec* FindInstallPlatformSpec(std::string_view platform);

std::string RenderInstallScript(std::string_view platform,
                                const std::filesystem::path& web_static_root);

std::string BuildInstallCommand(std::string_view platform,
                                const InstallCommandRequest& request);

std::string InstallScriptUrl(std::string_view platform,
                             const std::string& server_url);

}  // namespace zfleet::server
