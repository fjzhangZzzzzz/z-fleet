#pragma once

#include <filesystem>
#include <string>

namespace zfleet::server {

class InstallScriptRenderer {
 public:
  explicit InstallScriptRenderer(std::filesystem::path web_static_root);

  std::string RenderLinuxScript() const;
  std::string RenderWindowsScript() const;

 private:
  std::string ReadScript(const std::filesystem::path& relative_path) const;

  std::filesystem::path root_;
};

}  // namespace zfleet::server
