#include "install_platform_registry.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace zfleet::server {
namespace {

std::string ShellQuote(std::string_view value) {
  std::string output("'");
  for (const auto ch : value) {
    if (ch == '\'') {
      output += "'\"'\"'";
    } else {
      output.push_back(ch);
    }
  }
  output.push_back('\'');
  return output;
}

std::string LinuxInstallCommand(const InstallCommandRequest& request) {
  const auto script_url = InstallScriptUrl("linux", request.server_url);
  return "curl -fsSL " + ShellQuote(script_url) +
         " | sudo bash -s -- \\\n"
         "  --server-url " +
         ShellQuote(request.server_url) + " \\\n  --control-url " +
         ShellQuote(request.control_url) + " \\\n  --token " +
         ShellQuote(request.token) + " \\\n  --channel " +
         ShellQuote(request.channel) + " \\\n  --root " +
         ShellQuote(request.root);
}

std::string WindowsInstallCommand(const InstallCommandRequest& request) {
  return "powershell -ExecutionPolicy Bypass -File " +
         ShellQuote(InstallScriptUrl("windows", request.server_url)) +
         " -ServerUrl " + ShellQuote(request.server_url) + " -ControlUrl " +
         ShellQuote(request.control_url) + " -Token " +
         ShellQuote(request.token) + " -Channel " +
         ShellQuote(request.channel) + " -Root " + ShellQuote(request.root);
}

const InstallPlatformSpec kSpecs[] = {
    {"linux", "scripts/install/linux.sh", "text/x-shellscript",
     &LinuxInstallCommand},
    {"windows", "scripts/install/windows.ps1", "text/plain",
     &WindowsInstallCommand},
};

std::string ReadScript(const std::filesystem::path& root,
                       const std::filesystem::path& relative_path) {
  std::ifstream stream(root / relative_path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("install script template is missing: " +
                             (root / relative_path).string());
  }
  std::ostringstream output;
  output << stream.rdbuf();
  return output.str();
}

}  // namespace

const InstallPlatformSpec* FindInstallPlatformSpec(std::string_view platform) {
  for (const auto& spec : kSpecs) {
    if (spec.platform == platform) {
      return &spec;
    }
  }
  return nullptr;
}

std::string RenderInstallScript(std::string_view platform,
                                const std::filesystem::path& web_static_root) {
  const auto* spec = FindInstallPlatformSpec(platform);
  if (spec == nullptr) {
    throw std::invalid_argument("unsupported install platform");
  }
  return ReadScript(web_static_root, spec->script_path);
}

std::string BuildInstallCommand(std::string_view platform,
                                const InstallCommandRequest& request) {
  const auto* spec = FindInstallPlatformSpec(platform);
  if (spec == nullptr || spec->build_command == nullptr) {
    throw std::invalid_argument("unsupported install platform");
  }
  return spec->build_command(request);
}

std::string InstallScriptUrl(std::string_view platform,
                             const std::string& server_url) {
  const auto* spec = FindInstallPlatformSpec(platform);
  if (spec == nullptr) {
    throw std::invalid_argument("unsupported install platform");
  }
  return server_url +
         "/api/v1/install/script?platform=" + std::string(platform);
}

}  // namespace zfleet::server
