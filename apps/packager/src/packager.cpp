#include "packager.h"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <system_error>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <thread>

#include <zfleet/package/archive.h>

namespace zfleet::packager {
namespace {

namespace fs = std::filesystem;

struct PackagePaths {
  fs::path package_dir;
  fs::path manifest_dir;
  fs::path payload_dir;
  fs::path payload_binary;
  fs::path manifest_path;
};

class ScopedDirectory {
 public:
  explicit ScopedDirectory(fs::path path) : path_(std::move(path)) {}
  ScopedDirectory(const ScopedDirectory&) = delete;
  ScopedDirectory& operator=(const ScopedDirectory&) = delete;
  ScopedDirectory(ScopedDirectory&&) = delete;
  ScopedDirectory& operator=(ScopedDirectory&&) = delete;

  ~ScopedDirectory() {
    if (path_.empty()) {
      return;
    }

    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
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

void SetExecutable(const fs::path& path) {
  const auto mask = fs::perms::owner_exec | fs::perms::group_exec |
                    fs::perms::others_exec;
  fs::permissions(path, mask, fs::perm_options::add);
}

std::string BuildManifestText(const std::string& component,
                              const std::string& version,
                              const std::string& min_installer_version,
                              const std::string& binary_name,
                              std::uintmax_t size_bytes,
                              const std::string& sha256_hex) {
  std::ostringstream stream;
  stream << "{\n";
  stream << "  \"schema_version\": 1,\n";
  stream << "  \"component\": " << JsonString(component) << ",\n";
  stream << "  \"version\": " << JsonString(version) << ",\n";
  stream << "  \"min_installer_version\": "
         << JsonString(min_installer_version) << ",\n";
  stream << "  \"files\": [\n";
  stream << "    {\n";
  stream << "      \"source\": "
         << JsonString(std::string("payload/bin/") + binary_name) << ",\n";
  stream << "      \"target\": "
         << JsonString(std::string("bin/") + binary_name)
         << ",\n";
  stream << "      \"size\": " << size_bytes << ",\n";
  stream << "      \"sha256\": " << JsonString(sha256_hex) << ",\n";
  stream << "      \"executable\": true\n";
  stream << "    }\n";
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
  const auto manifest_dir = package_dir / "META";
  const auto payload_dir = package_dir / "payload" / "bin";
  const auto binary_name = BinaryNameForComponent(component);
  return PackagePaths{
      .package_dir = package_dir,
      .manifest_dir = manifest_dir,
      .payload_dir = payload_dir,
      .payload_binary = payload_dir / binary_name,
      .manifest_path = manifest_dir / "manifest.json",
  };
}

fs::path CreateTemporaryRoot() {
  const auto base_dir = fs::temp_directory_path() / "zfleet-packager";
  fs::create_directories(base_dir);

  const auto process_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
  const auto timestamp = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());

  for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
    const auto candidate =
        base_dir / (std::to_string(timestamp) + "-" +
                    std::to_string(process_id) + "-" +
                    std::to_string(attempt));
    std::error_code error;
    if (fs::create_directories(candidate, error)) {
      return candidate;
    }
  }

  throw std::runtime_error("failed to create temporary package directory");
}

PackDirResult PackDirImpl(const PackDirOptions& options,
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

  const auto paths = BuildPackagePaths(component, options.version, output_dir_abs);

  const auto binary_status = fs::status(options.binary_path);
  if (!fs::exists(binary_status) || !fs::is_regular_file(binary_status)) {
    throw std::runtime_error("binary must be an existing regular file: " +
                             options.binary_path.string());
  }

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

  if (!fs::copy_file(options.binary_path, paths.payload_binary,
                     fs::copy_options::overwrite_existing)) {
    throw std::runtime_error("failed to copy binary into package");
  }
  SetExecutable(paths.payload_binary);

  const auto size_bytes = fs::file_size(paths.payload_binary);
  const auto sha256_hex = ComputeSha256Hex(paths.payload_binary);
  const auto manifest_text = BuildManifestText(component, options.version,
                                               options.min_installer_version,
                                               BinaryNameForComponent(component),
                                               size_bytes, sha256_hex);

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

  return PackDirResult{.package_dir = paths.package_dir};
}

fs::path ArchivePathForPackageDir(const fs::path& package_dir) {
  auto archive_name = package_dir.filename();
  archive_name += ".zip";
  return package_dir.parent_path() / archive_name;
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

PackDirResult PackDir(const PackDirOptions& options) {
  return PackDirImpl(options, NormalizePath(options.output_dir));
}

PackArchiveResult PackArchive(const PackArchiveOptions& options) {
  const auto component = ValidateComponent(options.component);
  if (!IsSafeSingleSegment(options.version)) {
    throw std::invalid_argument(
        "invalid --version; expected [A-Za-z0-9._-]+ and not . or ..");
  }
  if (!IsSafeSingleSegment(options.min_installer_version)) {
    throw std::invalid_argument(
        "invalid --min-installer-version; expected [A-Za-z0-9._-]+ and not . or ..");
  }

  const auto output_dir_abs = NormalizePath(options.output_dir);
  const auto package_root = output_dir_abs / component;
  const auto package_dir = package_root / options.version;
  const auto archive_path = ArchivePathForPackageDir(package_dir);
  fs::create_directories(archive_path.parent_path());

  if (fs::exists(archive_path) && !options.force) {
    throw std::runtime_error("archive already exists: " + archive_path.string() +
                             " (use --force to overwrite)");
  }

  const auto temp_root = CreateTemporaryRoot();
  ScopedDirectory cleanup(temp_root);

  const auto pack_result = PackDirImpl(PackDirOptions{
                                          .component = options.component,
                                          .version = options.version,
                                          .binary_path = options.binary_path,
                                          .output_dir = temp_root,
                                          .min_installer_version =
                                              options.min_installer_version,
                                          .force = options.force,
                                      },
                                      temp_root);

  zfleet::package::CreateArchive(
      {.package_dir = pack_result.package_dir,
       .archive_path = archive_path,
       .force = options.force});

  return PackArchiveResult{.archive_path = NormalizePath(archive_path)};
}

ArchiveDirResult ArchiveDir(const ArchiveDirOptions& options) {
  const auto package_dir = NormalizePath(options.package_dir);
  const auto archive_path = NormalizePath(options.archive_path);

  const auto package_status = fs::status(package_dir);
  if (!fs::exists(package_status) || !fs::is_directory(package_status)) {
    throw std::runtime_error("package must be an existing directory: " +
                             package_dir.string());
  }

  fs::create_directories(archive_path.parent_path());
  if (fs::exists(archive_path) && !options.force) {
    throw std::runtime_error("archive already exists: " + archive_path.string() +
                             " (use --force to overwrite)");
  }

  zfleet::package::CreateArchive(
      {.package_dir = package_dir,
       .archive_path = archive_path,
       .force = options.force});

  return ArchiveDirResult{.archive_path = archive_path};
}

} // namespace zfleet::packager
