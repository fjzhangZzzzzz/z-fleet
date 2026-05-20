#include "packager.h"

#include <openssl/evp.h>

#include <zfleet/package/archive.h>
#include <zfleet/package/temp_dir.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zfleet::packager {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kManifestRelativePath = "META/manifest.json";

struct PackagePaths {
  fs::path package_dir;
  fs::path manifest_dir;
  fs::path payload_dir;
  fs::path manifest_path;
};

struct PayloadFile {
  std::string relative_path;
  fs::path source_path;
  std::uintmax_t size_bytes = 0;
  std::string sha256_hex;
  bool executable = false;
};

bool IsSafeSingleSegment(std::string_view value) {
  if (value.empty() || value == "." || value == "..") {
    return false;
  }

  for (const unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) {
      return false;
    }
  }
  return true;
}

bool IsWindowsDrivePrefix(std::string_view value) {
  return value.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
         value[1] == ':';
}

bool IsSafeRelativePath(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  if (value.front() == '/' || value.front() == '\\' ||
      value.back() == '/' || value.back() == '\\' ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  if (IsWindowsDrivePrefix(value)) {
    return false;
  }

  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t end = value.find('/', start);
    const auto part =
        value.substr(start, end == std::string_view::npos ? end : end - start);
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  return true;
}

std::string ComputeSha256Hex(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open file for sha256: " +
                             path.string());
  }

  using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  MdCtxPtr context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context) {
    throw std::runtime_error("failed to allocate sha256 context");
  }
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("failed to initialize sha256");
  }

  std::array<char, 8192> buffer{};
  while (stream.good()) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytes_read = stream.gcount();
    if (bytes_read <= 0) {
      break;
    }
    if (EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<std::size_t>(bytes_read)) != 1) {
      throw std::runtime_error("failed to update sha256");
    }
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_length) != 1) {
    throw std::runtime_error("failed to finalize sha256");
  }

  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(digest_length * 2);
  for (unsigned int index = 0; index < digest_length; ++index) {
    const auto value = digest[index];
    hex.push_back(kHexDigits[(value >> 4U) & 0x0fU]);
    hex.push_back(kHexDigits[value & 0x0fU]);
  }
  return hex;
}

std::string JsonString(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 2);
  output.push_back('"');
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (ch < 0x20) {
          static constexpr char kHexDigits[] = "0123456789abcdef";
          output += "\\u00";
          output.push_back(kHexDigits[(ch >> 4U) & 0x0fU]);
          output.push_back(kHexDigits[ch & 0x0fU]);
        } else {
          output.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  output.push_back('"');
  return output;
}

bool HasExecutablePermissions(const fs::perms permissions) {
#ifndef _WIN32
  return (permissions & fs::perms::owner_exec) != fs::perms::none ||
         (permissions & fs::perms::group_exec) != fs::perms::none ||
         (permissions & fs::perms::others_exec) != fs::perms::none;
#else
  (void)permissions;
  return false;
#endif
}

std::string BuildManifestText(const std::string& component,
                              const std::string& version,
                              const std::string& min_installer_version,
                              const std::vector<PayloadFile>& files) {
  std::ostringstream stream;
  stream << "{\n";
  stream << "  \"schema_version\": 1,\n";
  stream << "  \"component\": " << JsonString(component) << ",\n";
  stream << "  \"version\": " << JsonString(version) << ",\n";
  stream << "  \"min_installer_version\": "
         << JsonString(min_installer_version) << ",\n";
  stream << "  \"files\": [\n";
  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto& file = files[index];
    stream << "    {\n";
    stream << "      \"source\": "
           << JsonString(std::string("payload/") + file.relative_path)
           << ",\n";
    stream << "      \"target\": " << JsonString(file.relative_path) << ",\n";
    stream << "      \"size\": " << file.size_bytes << ",\n";
    stream << "      \"sha256\": " << JsonString(file.sha256_hex) << ",\n";
    stream << "      \"executable\": "
           << (file.executable ? "true" : "false") << "\n";
    stream << "    }" << (index + 1 == files.size() ? "\n" : ",\n");
  }
  stream << "  ],\n";
  stream << "  \"signatures\": []\n";
  stream << "}\n";
  return stream.str();
}

std::string ValidateComponent(std::string_view component) {
  if (component == "agent" || component == "server" ||
      component == "installer") {
    return std::string(component);
  }
  throw std::invalid_argument(
      "invalid --component; expected agent, server, or installer");
}

fs::path NormalizePath(const fs::path& path) {
  return fs::absolute(path).lexically_normal();
}

PackagePaths BuildPackagePaths(const std::string& component,
                               const std::string& version,
                               const fs::path& output_dir) {
  const auto package_dir = output_dir / component / version;
  return PackagePaths{
      .package_dir = package_dir,
      .manifest_dir = package_dir / "META",
      .payload_dir = package_dir / "payload",
      .manifest_path = package_dir / fs::path(kManifestRelativePath),
  };
}

fs::path ArchivePathForPackageDir(const fs::path& package_dir) {
  auto archive_name = package_dir.filename();
  archive_name += ".zip";
  return package_dir.parent_path() / archive_name;
}

void ValidatePayloadDir(const fs::path& payload_dir) {
  const auto status = fs::symlink_status(payload_dir);
  if (fs::is_symlink(status)) {
    throw std::runtime_error("payload directory cannot be a symlink: " +
                             payload_dir.string());
  }
  if (!fs::exists(status) || !fs::is_directory(status)) {
    throw std::runtime_error("payload directory must be an existing directory: " +
                             payload_dir.string());
  }
}

std::vector<PayloadFile> CollectPayloadFiles(const fs::path& payload_dir,
                                             std::string_view entry_path) {
  std::vector<PayloadFile> files;
  std::unordered_set<std::string> seen_paths;
  bool found_entry = false;

  for (fs::recursive_directory_iterator it(payload_dir), end; it != end; ++it) {
    const auto status = it->symlink_status();
    if (fs::is_symlink(status)) {
      throw std::runtime_error("symlink is not allowed in payload directory: " +
                               it->path().string());
    }
    if (fs::is_directory(status)) {
      continue;
    }
    if (!fs::is_regular_file(status)) {
      throw std::runtime_error("unsupported file type in payload directory: " +
                               it->path().string());
    }

    const auto relative_path = it->path().lexically_relative(payload_dir);
    const auto relative_string = relative_path.generic_string();
    if (!IsSafeRelativePath(relative_string)) {
      throw std::runtime_error("payload path is not safe: " + relative_string);
    }
    if (relative_string == "META" || relative_string.starts_with("META/")) {
      throw std::runtime_error("payload target must not write META: " +
                               relative_string);
    }
    if (!seen_paths.insert(relative_string).second) {
      throw std::runtime_error("duplicate payload path: " + relative_string);
    }

    PayloadFile file;
    file.relative_path = relative_string;
    file.source_path = it->path();
    file.size_bytes = fs::file_size(it->path());
    file.sha256_hex = ComputeSha256Hex(it->path());
    file.executable = relative_string == entry_path ||
                      HasExecutablePermissions(fs::status(it->path()).permissions());
    if (relative_string == entry_path) {
      found_entry = true;
    }
    files.push_back(std::move(file));
  }

  std::sort(files.begin(), files.end(),
            [](const PayloadFile& lhs, const PayloadFile& rhs) {
              return lhs.relative_path < rhs.relative_path;
            });

  if (files.empty()) {
    throw std::runtime_error("payload directory must contain at least one file");
  }
  if (!found_entry) {
    throw std::runtime_error("entry file is missing from payload directory: " +
                             std::string(entry_path));
  }
  return files;
}

void CopyPayloadFiles(const fs::path& package_payload_dir,
                      const std::vector<PayloadFile>& files) {
  for (const auto& file : files) {
    const auto target_path = package_payload_dir / fs::path(file.relative_path);
    if (const auto parent = target_path.parent_path(); !parent.empty()) {
      fs::create_directories(parent);
    }
    if (!fs::copy_file(file.source_path, target_path,
                       fs::copy_options::overwrite_existing)) {
      throw std::runtime_error("failed to copy payload file: " +
                               file.source_path.string());
    }
#ifndef _WIN32
    if (file.executable) {
      fs::permissions(target_path,
                      fs::perms::owner_exec | fs::perms::group_exec |
                          fs::perms::others_exec,
                      fs::perm_options::add);
    }
#endif
  }
}

PackResult PackToDirectory(const PackOptions& options,
                           const fs::path& output_dir_abs) {
  const auto component = ValidateComponent(options.component);
  if (!IsSafeSingleSegment(options.version)) {
    throw std::invalid_argument(
        "invalid --version; expected [A-Za-z0-9._-]+ and not . or ..");
  }
  if (!IsSafeSingleSegment(options.min_installer_version)) {
    throw std::invalid_argument(
        "invalid --min-installer-version; expected [A-Za-z0-9._-]+ and not . or ..");
  }

  const auto payload_dir = NormalizePath(options.payload_dir);
  ValidatePayloadDir(payload_dir);

  const auto entry_string = options.entry_path.generic_string();
  if (!IsSafeRelativePath(entry_string)) {
    throw std::invalid_argument("invalid --entry; expected safe relative path");
  }

  const auto files = CollectPayloadFiles(payload_dir, entry_string);
  const auto paths = BuildPackagePaths(component, options.version, output_dir_abs);

  if (fs::exists(paths.package_dir)) {
    if (!options.force) {
      throw std::runtime_error("package already exists: " +
                               paths.package_dir.string() +
                               " (use --force to overwrite)");
    }
    fs::remove_all(paths.package_dir);
  }

  fs::create_directories(paths.manifest_dir);
  fs::create_directories(paths.payload_dir);
  CopyPayloadFiles(paths.payload_dir, files);

  const auto manifest_text =
      BuildManifestText(component, options.version,
                        options.min_installer_version, files);
  std::ofstream manifest_stream(paths.manifest_path, std::ios::binary);
  if (!manifest_stream) {
    throw std::runtime_error("failed to open manifest for write: " +
                             paths.manifest_path.string());
  }
  manifest_stream << manifest_text;
  if (!manifest_stream) {
    throw std::runtime_error("failed to write manifest: " +
                             paths.manifest_path.string());
  }

  return PackResult{.package_path = paths.package_dir, .archive = false};
}

} // namespace

bool IsSafeSegment(std::string_view value) {
  return IsSafeSingleSegment(value);
}

bool IsKnownComponent(std::string_view component) {
  return component == "agent" || component == "server" ||
         component == "installer";
}

std::string BinaryNameForComponent(std::string_view component) {
  if (!IsKnownComponent(component)) {
    throw std::invalid_argument("unknown component: " + std::string(component));
  }

#ifdef _WIN32
  return "zfleet_" + std::string(component) + ".exe";
#else
  return "zfleet_" + std::string(component);
#endif
}

PackResult Pack(const PackOptions& options) {
  const auto output_dir_abs = NormalizePath(options.output_dir);
  if (!options.archive) {
    return PackToDirectory(options, output_dir_abs);
  }

  const auto component = ValidateComponent(options.component);
  if (!IsSafeSingleSegment(options.version)) {
    throw std::invalid_argument(
        "invalid --version; expected [A-Za-z0-9._-]+ and not . or ..");
  }

  const auto archive_path =
      ArchivePathForPackageDir(output_dir_abs / component / options.version);
  fs::create_directories(archive_path.parent_path());
  if (fs::exists(archive_path) && !options.force) {
    throw std::runtime_error("archive already exists: " + archive_path.string() +
                             " (use --force to overwrite)");
  }

  const zfleet::package::ScopedTempDir temp_root("zfleet-packager");
  const auto pack_result =
      PackToDirectory(PackOptions{
                          .component = options.component,
                          .version = options.version,
                          .payload_dir = options.payload_dir,
                          .entry_path = options.entry_path,
                          .output_dir = temp_root.path(),
                          .min_installer_version =
                              options.min_installer_version,
                          .archive = false,
                          .force = true,
                      },
                      temp_root.path());

  zfleet::package::CreateArchive(zfleet::package::CreateArchiveOptions{
      .package_dir = pack_result.package_path,
      .archive_path = archive_path,
      .force = options.force,
  });
  return PackResult{.package_path = NormalizePath(archive_path), .archive = true};
}

} // namespace zfleet::packager
