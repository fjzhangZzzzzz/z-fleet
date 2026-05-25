#include "install_script_renderer.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace zfleet::server {

InstallScriptRenderer::InstallScriptRenderer(std::filesystem::path web_static_root)
    : root_(std::move(web_static_root)) {}

std::string InstallScriptRenderer::RenderLinuxScript() const {
  return ReadScript("scripts/install/linux.sh");
}

std::string InstallScriptRenderer::RenderWindowsScript() const {
  return ReadScript("scripts/install/windows.ps1");
}

std::string InstallScriptRenderer::ReadScript(
    const std::filesystem::path& relative_path) const {
  std::ifstream stream(root_ / relative_path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("install script template is missing: " +
                             (root_ / relative_path).string());
  }
  std::ostringstream output;
  output << stream.rdbuf();
  return output.str();
}

}  // namespace zfleet::server
