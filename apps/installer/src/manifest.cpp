#include "json_codec.h"
#include "manifest.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace zfleet::installer {
namespace {

namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open file: " + path.string());
  }

  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

} // namespace

Manifest LoadManifest(const fs::path& manifest_path) {
  return ParseManifestJson(ReadFile(manifest_path));
}

bool IsExecutable(const fs::path& path) {
  const auto permissions = fs::status(path).permissions();
  return (permissions & fs::perms::owner_exec) != fs::perms::none ||
         (permissions & fs::perms::group_exec) != fs::perms::none ||
         (permissions & fs::perms::others_exec) != fs::perms::none;
}

void SetExecutable(const fs::path& path, bool executable) {
  const auto mask = fs::perms::owner_exec | fs::perms::group_exec |
                    fs::perms::others_exec;
  fs::permissions(path, mask,
                  executable ? fs::perm_options::add
                             : fs::perm_options::remove);
}

} // namespace zfleet::installer
