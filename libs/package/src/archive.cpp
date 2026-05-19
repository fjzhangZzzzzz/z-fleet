#include "zfleet/package/archive.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zfleet::package {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferSize = 64 * 1024;
constexpr std::uint32_t kLocalFileHeaderSignature = 0x04034b50U;
constexpr std::uint32_t kCentralDirectorySignature = 0x02014b50U;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50U;
constexpr std::uint16_t kZipVersion = 20U;
constexpr std::uint16_t kGeneralPurposeUtf8 = 0x0800U;
constexpr std::uint16_t kStoredCompressionMethod = 0U;
constexpr std::uint16_t kDeflatedCompressionMethod = 8U;
constexpr std::uint8_t kUnixOsCode = 3U;
constexpr std::uint32_t kUnixFileTypeMask = 0170000U;
constexpr std::uint32_t kUnixRegularFileType = 0100000U;
constexpr std::uint32_t kUnixSymlinkFileType = 0120000U;
constexpr std::string_view kManifestPath = "META/manifest.json";

struct SourceEntry {
  std::string path;
  fs::path source_path;
  bool executable = false;
  std::uint64_t uncompressed_size = 0;
};

struct ArchiveRecord {
  std::string path;
  bool executable = false;
  std::uint64_t compressed_size = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint64_t local_header_offset = 0;
  std::uint32_t crc32 = 0;
  std::uint16_t compression_method = 0;
  std::uint16_t general_purpose_bit_flag = 0;
  std::uint16_t version_made_by = 0;
};

struct EndOfCentralDirectory {
  std::uint64_t central_directory_offset = 0;
  std::uint64_t central_directory_size = 0;
  std::uint16_t entry_count = 0;
};

struct FileWriteStats {
  std::uint32_t crc32 = 0;
  std::uint64_t compressed_size = 0;
  std::uint64_t uncompressed_size = 0;
};

class ByteReader {
 public:
  explicit ByteReader(const std::vector<std::uint8_t>& data, std::size_t offset = 0)
      : data_(data), offset_(offset) {}

  std::size_t Remaining() const noexcept { return data_.size() - offset_; }

  std::uint16_t ReadU16() {
    Require(sizeof(std::uint16_t));
    const std::uint16_t value =
        static_cast<std::uint16_t>(data_[offset_]) |
        (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
    offset_ += sizeof(std::uint16_t);
    return value;
  }

  std::uint32_t ReadU32() {
    Require(sizeof(std::uint32_t));
    const std::uint32_t value =
        static_cast<std::uint32_t>(data_[offset_]) |
        (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8) |
        (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16) |
        (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24);
    offset_ += sizeof(std::uint32_t);
    return value;
  }

  std::uint64_t ReadU64() {
    Require(sizeof(std::uint64_t));
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(data_[offset_ + index]) << (8 * index);
    }
    offset_ += sizeof(std::uint64_t);
    return value;
  }

  std::string ReadString(std::size_t size) {
    Require(size);
    std::string value(reinterpret_cast<const char*>(data_.data() + offset_), size);
    offset_ += size;
    return value;
  }

  void Skip(std::size_t size) {
    Require(size);
    offset_ += size;
  }

 private:
  void Require(std::size_t size) const {
    if (size > Remaining()) {
      throw std::runtime_error("archive payload is truncated");
    }
  }

  const std::vector<std::uint8_t>& data_;
  std::size_t offset_ = 0;
};

bool IsWindowsDrivePrefix(std::string_view value) {
  return value.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
         value[1] == ':';
}

bool IsSafeRelativeArchivePath(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  if (value.front() == '/' || value.front() == '\\' || value.back() == '/' ||
      value.back() == '\\' || value.find('\\') != std::string_view::npos) {
    return false;
  }
  if (IsWindowsDrivePrefix(value)) {
    return false;
  }

  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t end = value.find('/', start);
    const std::string_view part =
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

bool IsWithinBase(const fs::path& base, const fs::path& candidate) {
  const auto base_string = base.generic_string();
  const auto candidate_string = candidate.generic_string();
  if (candidate_string == base_string) {
    return true;
  }
  if (candidate_string.size() <= base_string.size()) {
    return false;
  }
  if (candidate_string.compare(0, base_string.size(), base_string) != 0) {
    return false;
  }
  if (!base_string.empty() && base_string.back() == '/') {
    return true;
  }
  return candidate_string[base_string.size()] == '/';
}

void WriteU16(std::ostream& stream, std::uint16_t value) {
  const std::array<char, 2> bytes{
      static_cast<char>(value & 0xffU),
      static_cast<char>((value >> 8) & 0xffU),
  };
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void WriteU32(std::ostream& stream, std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xffU),
      static_cast<char>((value >> 8) & 0xffU),
      static_cast<char>((value >> 16) & 0xffU),
      static_cast<char>((value >> 24) & 0xffU),
  };
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void EnsureStreamOk(const std::ios& stream, const std::string& message) {
  if (!stream) {
    throw std::runtime_error(message);
  }
}

std::vector<std::uint8_t> ReadFileRange(const fs::path& path,
                                        std::uint64_t offset,
                                        std::uint64_t size) {
  if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error("requested range is too large: " + path.string());
  }

  std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open file: " + path.string());
  }
  stream.seekg(static_cast<std::streamoff>(offset));
  EnsureStreamOk(stream, "failed to seek in file: " + path.string());
  if (!contents.empty()) {
    stream.read(reinterpret_cast<char*>(contents.data()),
                static_cast<std::streamsize>(contents.size()));
    if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
      throw std::runtime_error("failed to read file range: " + path.string());
    }
  }
  return contents;
}

std::uint64_t GetFileSize(const fs::path& path) {
  const auto size = fs::file_size(path);
  return static_cast<std::uint64_t>(size);
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

std::vector<SourceEntry> CollectSourceEntries(const fs::path& package_dir) {
  std::vector<SourceEntry> entries;
  std::unordered_set<std::string> seen_paths;

  for (fs::recursive_directory_iterator it(package_dir), end; it != end; ++it) {
    const auto status = it->symlink_status();
    if (fs::is_symlink(status)) {
      throw std::runtime_error("symlink is not allowed in package directory: " +
                               it->path().string());
    }
    if (fs::is_directory(status)) {
      continue;
    }
    if (!fs::is_regular_file(status)) {
      throw std::runtime_error("unsupported file type in package directory: " +
                               it->path().string());
    }

    const auto relative_path = it->path().lexically_relative(package_dir);
    const auto relative_string = relative_path.generic_string();
    if (!IsSafeRelativeArchivePath(relative_string)) {
      throw std::runtime_error("package path is not safe: " + relative_string);
    }
    if (!seen_paths.insert(relative_string).second) {
      throw std::runtime_error("duplicate package path: " + relative_string);
    }
    if (relative_string.size() > std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error("archive path is too long: " + relative_string);
    }

    const auto file_size = GetFileSize(it->path());
    if (file_size > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("file is too large for ZIP32: " + relative_string);
    }

    SourceEntry entry;
    entry.path = relative_string;
    entry.source_path = it->path();
    entry.uncompressed_size = file_size;
#ifndef _WIN32
    entry.executable = HasExecutablePermissions(fs::status(it->path()).permissions());
#else
    entry.executable = false;
#endif
    entries.push_back(std::move(entry));
  }

  std::sort(entries.begin(), entries.end(),
            [](const SourceEntry& lhs, const SourceEntry& rhs) {
              return lhs.path < rhs.path;
            });
  return entries;
}

void WriteLocalFileHeader(std::ostream& stream, const SourceEntry& entry) {
  WriteU32(stream, kLocalFileHeaderSignature);
  WriteU16(stream, kZipVersion);
  WriteU16(stream, kGeneralPurposeUtf8);
  WriteU16(stream, kDeflatedCompressionMethod);
  WriteU16(stream, 0U);
  WriteU16(stream, 0U);
  WriteU32(stream, 0U);
  WriteU32(stream, 0U);
  WriteU32(stream, 0U);
  WriteU16(stream, static_cast<std::uint16_t>(entry.path.size()));
  WriteU16(stream, 0U);
  stream.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
}

void PatchLocalFileHeader(std::ostream& stream, std::streamoff header_offset,
                          const FileWriteStats& stats) {
  const auto current_position = stream.tellp();
  if (current_position == std::streampos(-1)) {
    throw std::runtime_error("failed to query archive position");
  }

  stream.seekp(header_offset + static_cast<std::streamoff>(14));
  EnsureStreamOk(stream, "failed to seek archive for local header patch");
  WriteU32(stream, stats.crc32);
  WriteU32(stream, static_cast<std::uint32_t>(stats.compressed_size));
  WriteU32(stream, static_cast<std::uint32_t>(stats.uncompressed_size));
  EnsureStreamOk(stream, "failed to patch local header");

  stream.seekp(current_position);
  EnsureStreamOk(stream, "failed to restore archive position");
}

FileWriteStats WriteDeflatedFileData(const fs::path& source_path, std::ostream& stream) {
  std::ifstream input(source_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file: " + source_path.string());
  }

  z_stream zstream{};
  const int init_result =
      deflateInit2(&zstream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                   Z_DEFAULT_STRATEGY);
  if (init_result != Z_OK) {
    throw std::runtime_error("failed to initialize zlib deflate");
  }

  std::array<std::uint8_t, kBufferSize> input_buffer{};
  std::array<std::uint8_t, kBufferSize> output_buffer{};
  FileWriteStats stats{};
  stats.crc32 = crc32(0L, Z_NULL, 0);

  bool finished_input = false;
  while (true) {
    if (zstream.avail_in == 0 && !finished_input) {
      input.read(reinterpret_cast<char*>(input_buffer.data()),
                 static_cast<std::streamsize>(input_buffer.size()));
      const std::streamsize bytes_read = input.gcount();
      if (bytes_read < 0) {
        deflateEnd(&zstream);
        throw std::runtime_error("failed to read file: " + source_path.string());
      }
      if (bytes_read == 0) {
        if (!input.eof()) {
          deflateEnd(&zstream);
          throw std::runtime_error("failed to read file: " + source_path.string());
        }
        finished_input = true;
        zstream.next_in = Z_NULL;
        zstream.avail_in = 0;
      } else {
        stats.uncompressed_size += static_cast<std::uint64_t>(bytes_read);
        if (stats.uncompressed_size > std::numeric_limits<std::uint32_t>::max()) {
          deflateEnd(&zstream);
          throw std::runtime_error("file is too large for ZIP32: " + source_path.string());
        }
        stats.crc32 = crc32(stats.crc32, input_buffer.data(),
                            static_cast<uInt>(bytes_read));
        zstream.next_in = reinterpret_cast<Bytef*>(input_buffer.data());
        zstream.avail_in = static_cast<uInt>(bytes_read);
        if (input.eof()) {
          finished_input = true;
        }
      }
    }

    const int flush = finished_input ? Z_FINISH : Z_NO_FLUSH;
    int result = Z_OK;
    do {
      zstream.next_out = reinterpret_cast<Bytef*>(output_buffer.data());
      zstream.avail_out = static_cast<uInt>(output_buffer.size());
      result = deflate(&zstream, flush);
      if (result == Z_STREAM_ERROR) {
        deflateEnd(&zstream);
        throw std::runtime_error("failed to deflate archive entry");
      }

      const auto produced = output_buffer.size() - zstream.avail_out;
      if (produced > 0) {
        stream.write(reinterpret_cast<const char*>(output_buffer.data()),
                     static_cast<std::streamsize>(produced));
        EnsureStreamOk(stream, "failed to write archive entry");
        stats.compressed_size += static_cast<std::uint64_t>(produced);
        if (stats.compressed_size > std::numeric_limits<std::uint32_t>::max()) {
          deflateEnd(&zstream);
          throw std::runtime_error("compressed file is too large for ZIP32: " +
                                   source_path.string());
        }
      }
    } while (zstream.avail_out == 0);

    if (result == Z_STREAM_END) {
      break;
    }
    if (flush == Z_FINISH && zstream.avail_in == 0 && finished_input) {
      continue;
    }
    if (result != Z_OK) {
      deflateEnd(&zstream);
      throw std::runtime_error("failed to deflate archive entry");
    }
  }

  deflateEnd(&zstream);
  return stats;
}

std::uint32_t FileModeForEntry(bool executable) {
#ifndef _WIN32
  return static_cast<std::uint32_t>(executable ? 0100755U : 0100644U);
#else
  (void)executable;
  return 0100644U;
#endif
}

void WriteCentralDirectoryEntry(std::ostream& stream, const ArchiveRecord& entry) {
  WriteU32(stream, kCentralDirectorySignature);
  WriteU16(stream, static_cast<std::uint16_t>((kUnixOsCode << 8) | kZipVersion));
  WriteU16(stream, kZipVersion);
  WriteU16(stream, entry.general_purpose_bit_flag);
  WriteU16(stream, entry.compression_method);
  WriteU16(stream, 0U);
  WriteU16(stream, 0U);
  WriteU32(stream, entry.crc32);
  WriteU32(stream, static_cast<std::uint32_t>(entry.compressed_size));
  WriteU32(stream, static_cast<std::uint32_t>(entry.uncompressed_size));
  WriteU16(stream, static_cast<std::uint16_t>(entry.path.size()));
  WriteU16(stream, 0U);
  WriteU16(stream, 0U);
  WriteU16(stream, 0U);
  WriteU16(stream, 0U);
  WriteU32(stream, static_cast<std::uint32_t>(FileModeForEntry(entry.executable) << 16));
  WriteU32(stream, static_cast<std::uint32_t>(entry.local_header_offset));
  stream.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
}

std::uint32_t ReadU32FromTail(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint16_t ReadU16FromTail(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         (static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

EndOfCentralDirectory LocateEndOfCentralDirectory(const fs::path& archive_path) {
  const auto file_size = GetFileSize(archive_path);
  if (file_size < 22U) {
    throw std::runtime_error("archive is too small to be a ZIP file: " +
                             archive_path.string());
  }

  const std::uint64_t tail_size = std::min<std::uint64_t>(file_size, 22U + 0xffffU);
  const auto tail =
      ReadFileRange(archive_path, file_size - tail_size, tail_size);
  if (tail.size() < 22U) {
    throw std::runtime_error("archive is too small to contain EOCD: " +
                             archive_path.string());
  }

  for (std::size_t index = tail.size() - 22U + 1U; index-- > 0;) {
    if (ReadU32FromTail(tail, index) != kEndOfCentralDirectorySignature) {
      continue;
    }

    EndOfCentralDirectory eocd;
    const std::size_t offset = index + 4U;
    const std::uint16_t disk_number = ReadU16FromTail(tail, offset);
    const std::uint16_t central_directory_disk = ReadU16FromTail(tail, offset + 2U);
    const std::uint16_t entries_on_disk = ReadU16FromTail(tail, offset + 4U);
    const std::uint16_t total_entries = ReadU16FromTail(tail, offset + 6U);
    const std::uint32_t central_directory_size = ReadU32FromTail(tail, offset + 8U);
    const std::uint32_t central_directory_offset = ReadU32FromTail(tail, offset + 12U);
    const std::uint16_t comment_length = ReadU16FromTail(tail, offset + 16U);

    if (index + 22U + comment_length != tail.size()) {
      continue;
    }
    if (disk_number != 0U || central_directory_disk != 0U ||
        entries_on_disk != total_entries) {
      throw std::runtime_error("multi-disk ZIP archives are not supported: " +
                               archive_path.string());
    }
    if (central_directory_size == std::numeric_limits<std::uint32_t>::max() ||
        central_directory_offset == std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("ZIP64 archives are not supported: " + archive_path.string());
    }

    eocd.central_directory_size = central_directory_size;
    eocd.central_directory_offset = central_directory_offset;
    eocd.entry_count = total_entries;
    return eocd;
  }

  throw std::runtime_error("archive is missing the EOCD record: " + archive_path.string());
}

std::vector<ArchiveRecord> ParseCentralDirectory(const fs::path& archive_path) {
  const auto eocd = LocateEndOfCentralDirectory(archive_path);
  const auto archive_size = GetFileSize(archive_path);
  if (eocd.central_directory_offset + eocd.central_directory_size > archive_size) {
    throw std::runtime_error("central directory is truncated: " + archive_path.string());
  }

  const auto central_directory = ReadFileRange(archive_path, eocd.central_directory_offset,
                                               eocd.central_directory_size);
  ByteReader reader(central_directory);

  std::vector<ArchiveRecord> entries;
  entries.reserve(eocd.entry_count);
  std::unordered_set<std::string> seen_paths;

  for (std::uint16_t index = 0; index < eocd.entry_count; ++index) {
    if (reader.Remaining() < 46U) {
      throw std::runtime_error("central directory entry is truncated: " +
                               archive_path.string());
    }

    const std::uint32_t signature = reader.ReadU32();
    if (signature != kCentralDirectorySignature) {
      throw std::runtime_error("central directory signature mismatch: " +
                               archive_path.string());
    }

    const std::uint16_t version_made_by = reader.ReadU16();
    const std::uint16_t version_needed = reader.ReadU16();
    const std::uint16_t flags = reader.ReadU16();
    const std::uint16_t compression_method = reader.ReadU16();
    reader.Skip(4U);
    const std::uint32_t crc32_value = reader.ReadU32();
    const std::uint32_t compressed_size = reader.ReadU32();
    const std::uint32_t uncompressed_size = reader.ReadU32();
    const std::uint16_t name_length = reader.ReadU16();
    const std::uint16_t extra_length = reader.ReadU16();
    const std::uint16_t comment_length = reader.ReadU16();
    reader.Skip(4U);
    const std::uint32_t external_attributes = reader.ReadU32();
    const std::uint32_t local_header_offset = reader.ReadU32();
    const std::string path = reader.ReadString(name_length);
    reader.Skip(extra_length);
    reader.Skip(comment_length);

    if ((flags & 0x0001U) != 0U) {
      throw std::runtime_error("encrypted ZIP entries are not supported: " + path);
    }
    if ((flags & 0x0008U) != 0U) {
      throw std::runtime_error("ZIP data descriptors are not supported: " + path);
    }
    if ((flags & 0x0040U) != 0U) {
      throw std::runtime_error("strongly encrypted ZIP entries are not supported: " + path);
    }
    if (compression_method != kStoredCompressionMethod &&
        compression_method != kDeflatedCompressionMethod) {
      throw std::runtime_error("unsupported ZIP compression method: " + path);
    }
    if (compressed_size == std::numeric_limits<std::uint32_t>::max() ||
        uncompressed_size == std::numeric_limits<std::uint32_t>::max() ||
        local_header_offset == std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("ZIP64 archives are not supported: " + path);
    }
    if (!IsSafeRelativeArchivePath(path)) {
      throw std::runtime_error("archive entry path is not safe: " + path);
    }
    if (!seen_paths.insert(path).second) {
      throw std::runtime_error("duplicate archive entry path: " + path);
    }
    if (local_header_offset >= archive_size) {
      throw std::runtime_error("archive entry points outside the ZIP file: " + path);
    }
    if (compression_method == kStoredCompressionMethod &&
        compressed_size != uncompressed_size) {
      throw std::runtime_error("stored ZIP entry has mismatched sizes: " + path);
    }

    ArchiveRecord entry;
    entry.path = path;
    entry.compression_method = compression_method;
    entry.general_purpose_bit_flag = flags;
    entry.crc32 = crc32_value;
    entry.compressed_size = compressed_size;
    entry.uncompressed_size = uncompressed_size;
    entry.local_header_offset = local_header_offset;
    entry.version_made_by = version_made_by;
    if ((version_made_by >> 8) == kUnixOsCode) {
      const std::uint32_t unix_mode = external_attributes >> 16;
      const std::uint32_t file_type = unix_mode & kUnixFileTypeMask;
      if (file_type == kUnixSymlinkFileType) {
        throw std::runtime_error("symlink ZIP entries are not supported: " + path);
      }
      if (file_type != 0U && file_type != kUnixRegularFileType) {
        throw std::runtime_error("unsupported ZIP entry file type: " + path);
      }
      entry.executable = (unix_mode & 0111U) != 0U;
    }
    entries.push_back(std::move(entry));
  }

  if (reader.Remaining() != 0U) {
    throw std::runtime_error("central directory contains trailing data: " +
                             archive_path.string());
  }

  return entries;
}

struct LocalFileInfo {
  std::uint16_t compression_method = 0;
  std::uint16_t flags = 0;
  std::uint32_t crc32 = 0;
  std::uint32_t compressed_size = 0;
  std::uint32_t uncompressed_size = 0;
  std::uint64_t data_offset = 0;
};

LocalFileInfo ReadLocalFileInfo(std::ifstream& stream, const fs::path& archive_path,
                                const ArchiveRecord& entry) {
  stream.clear();
  stream.seekg(static_cast<std::streamoff>(entry.local_header_offset));
  EnsureStreamOk(stream, "failed to seek within archive: " + archive_path.string());

  std::array<std::uint8_t, 30> header{};
  stream.read(reinterpret_cast<char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
  if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
    throw std::runtime_error("local file header is truncated: " + entry.path);
  }

  const std::vector<std::uint8_t> header_bytes(header.begin(), header.end());
  ByteReader reader(header_bytes);
  const std::uint32_t signature = reader.ReadU32();
  if (signature != kLocalFileHeaderSignature) {
    throw std::runtime_error("local file header signature mismatch: " + entry.path);
  }
  reader.Skip(2U);

  LocalFileInfo info;
  info.flags = reader.ReadU16();
  info.compression_method = reader.ReadU16();
  reader.Skip(4U);
  info.crc32 = reader.ReadU32();
  info.compressed_size = reader.ReadU32();
  info.uncompressed_size = reader.ReadU32();
  const std::uint16_t name_length = reader.ReadU16();
  const std::uint16_t extra_length = reader.ReadU16();

  if ((info.flags & 0x0001U) != 0U) {
    throw std::runtime_error("encrypted ZIP entries are not supported: " + entry.path);
  }
  if ((info.flags & 0x0008U) != 0U) {
    throw std::runtime_error("ZIP data descriptors are not supported: " + entry.path);
  }
  if (info.compression_method != entry.compression_method) {
    throw std::runtime_error("ZIP compression method mismatch: " + entry.path);
  }
  if (info.crc32 != entry.crc32 || info.compressed_size != entry.compressed_size ||
      info.uncompressed_size != entry.uncompressed_size) {
    throw std::runtime_error("ZIP local header metadata mismatch: " + entry.path);
  }

  std::vector<std::uint8_t> name_bytes(name_length);
  if (name_length > 0U) {
    stream.read(reinterpret_cast<char*>(name_bytes.data()),
                static_cast<std::streamsize>(name_bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(name_bytes.size())) {
      throw std::runtime_error("local file name is truncated: " + entry.path);
    }
  }
  const std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
  if (name != entry.path) {
    throw std::runtime_error("ZIP local header path mismatch: " + entry.path);
  }

  if (extra_length > 0U) {
    stream.ignore(static_cast<std::streamsize>(extra_length));
    if (!stream) {
      throw std::runtime_error("ZIP local header extra field is truncated: " + entry.path);
    }
  }

  info.data_offset = entry.local_header_offset + 30U + name_length + extra_length;
  const auto archive_size = GetFileSize(archive_path);
  if (info.data_offset + entry.compressed_size > archive_size) {
    throw std::runtime_error("ZIP entry is truncated: " + entry.path);
  }
  return info;
}

template <typename Sink>
void CopyStoredEntry(std::ifstream& archive, const ArchiveRecord& entry, Sink&& sink) {
  std::array<std::uint8_t, kBufferSize> buffer{};
  std::uint64_t remaining = entry.compressed_size;
  std::uint64_t written = 0;
  while (remaining > 0U) {
    const std::size_t chunk_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    archive.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(chunk_size));
    if (archive.gcount() != static_cast<std::streamsize>(chunk_size)) {
      throw std::runtime_error("stored ZIP entry is truncated: " + entry.path);
    }
    sink(buffer.data(), chunk_size);
    remaining -= chunk_size;
    written += chunk_size;
  }
  if (written != entry.uncompressed_size) {
    throw std::runtime_error("stored ZIP entry size mismatch: " + entry.path);
  }
}

template <typename Sink>
void InflateDeflatedEntry(std::ifstream& archive, const ArchiveRecord& entry,
                          Sink&& sink) {
  z_stream zstream{};
  const int init_result = inflateInit2(&zstream, -MAX_WBITS);
  if (init_result != Z_OK) {
    throw std::runtime_error("failed to initialize zlib inflate");
  }

  std::array<std::uint8_t, kBufferSize> input_buffer{};
  std::array<std::uint8_t, kBufferSize> output_buffer{};
  std::uint64_t remaining = entry.compressed_size;
  bool finished_input = remaining == 0U;
  int result = Z_OK;
  std::uint64_t written = 0;

  while (true) {
    if (zstream.avail_in == 0 && !finished_input) {
      const std::size_t chunk_size = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, input_buffer.size()));
      archive.read(reinterpret_cast<char*>(input_buffer.data()),
                   static_cast<std::streamsize>(chunk_size));
      if (archive.gcount() != static_cast<std::streamsize>(chunk_size)) {
        inflateEnd(&zstream);
        throw std::runtime_error("deflated ZIP entry is truncated: " + entry.path);
      }
      remaining -= chunk_size;
      zstream.next_in = reinterpret_cast<Bytef*>(input_buffer.data());
      zstream.avail_in = static_cast<uInt>(chunk_size);
      finished_input = remaining == 0U;
    }

    do {
      zstream.next_out = reinterpret_cast<Bytef*>(output_buffer.data());
      zstream.avail_out = static_cast<uInt>(output_buffer.size());
      result = inflate(&zstream, Z_NO_FLUSH);
      if (result != Z_OK && result != Z_STREAM_END) {
        inflateEnd(&zstream);
        throw std::runtime_error("failed to inflate ZIP entry: " + entry.path);
      }

      const auto produced = output_buffer.size() - zstream.avail_out;
      if (produced > 0U) {
        sink(output_buffer.data(), produced);
        written += produced;
      }
    } while (zstream.avail_out == 0U);

    if (result == Z_STREAM_END) {
      if (remaining != 0U || zstream.avail_in != 0U) {
        inflateEnd(&zstream);
        throw std::runtime_error("deflated ZIP entry contains trailing data: " + entry.path);
      }
      break;
    }
    if (finished_input && zstream.avail_in == 0U) {
      inflateEnd(&zstream);
      throw std::runtime_error("deflated ZIP entry is truncated: " + entry.path);
    }
  }

  if (written != entry.uncompressed_size) {
    inflateEnd(&zstream);
    throw std::runtime_error("deflated ZIP entry size mismatch: " + entry.path);
  }

  inflateEnd(&zstream);
}

template <typename Sink>
void TransferArchiveEntryData(std::ifstream& archive, const fs::path& archive_path,
                              const ArchiveRecord& entry, Sink&& sink) {
  const LocalFileInfo info = ReadLocalFileInfo(archive, archive_path, entry);
  archive.clear();
  archive.seekg(static_cast<std::streamoff>(info.data_offset));
  EnsureStreamOk(archive, "failed to seek entry data");

  if (entry.compression_method == kStoredCompressionMethod) {
    CopyStoredEntry(archive, entry, std::forward<Sink>(sink));
    return;
  }
  if (entry.compression_method == kDeflatedCompressionMethod) {
    InflateDeflatedEntry(archive, entry, std::forward<Sink>(sink));
    return;
  }
  throw std::runtime_error("unsupported ZIP compression method: " + entry.path);
}

ArchiveRecord FindArchiveRecord(const std::vector<ArchiveRecord>& records,
                                std::string_view path) {
  const auto it = std::find_if(records.begin(), records.end(),
                               [path](const ArchiveRecord& record) {
                                 return record.path == path;
                               });
  if (it == records.end()) {
    throw std::runtime_error("archive entry not found: " + std::string(path));
  }
  return *it;
}

std::vector<std::uint8_t> ReadArchiveEntryBytes(const fs::path& archive_path,
                                               const ArchiveRecord& entry) {
  std::ifstream archive(archive_path, std::ios::binary);
  if (!archive) {
    throw std::runtime_error("failed to open file: " + archive_path.string());
  }

  std::vector<std::uint8_t> contents;
  if (entry.uncompressed_size > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("archive entry is too large to load: " + entry.path);
  }
  contents.reserve(static_cast<std::size_t>(entry.uncompressed_size));

  auto sink = [&contents](const std::uint8_t* data, std::size_t size) {
    contents.insert(contents.end(), data, data + size);
  };
  TransferArchiveEntryData(archive, archive_path, entry, sink);
  if (contents.size() != static_cast<std::size_t>(entry.uncompressed_size)) {
    throw std::runtime_error("archive entry size mismatch: " + entry.path);
  }
  return contents;
}

void ExtractArchiveEntry(const fs::path& archive_path, const ArchiveRecord& entry,
                         const fs::path& output_root) {
  const fs::path target_path = (output_root / fs::path(entry.path)).lexically_normal();
  if (!IsWithinBase(output_root.lexically_normal(), target_path)) {
    throw std::runtime_error("archive entry escapes output directory: " + entry.path);
  }

  if (const auto parent = target_path.parent_path(); !parent.empty()) {
    fs::create_directories(parent);
  }

  std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open file for write: " + target_path.string());
  }

  std::ifstream archive(archive_path, std::ios::binary);
  if (!archive) {
    throw std::runtime_error("failed to open file: " + archive_path.string());
  }

  auto sink = [&output](const std::uint8_t* data, std::size_t size) {
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    EnsureStreamOk(output, "failed to write extracted file");
  };
  TransferArchiveEntryData(archive, archive_path, entry, sink);

#ifndef _WIN32
  if (entry.executable) {
    fs::permissions(target_path,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add);
  }
#else
  (void)entry;
#endif
}

} // namespace

bool IsArchivePath(const fs::path& path) {
  return path.extension() == ".zip";
}

void CreateArchive(const CreateArchiveOptions& options) {
  if (options.package_dir.empty()) {
    throw std::runtime_error("package directory is empty");
  }
  if (options.archive_path.empty()) {
    throw std::runtime_error("archive path is empty");
  }
  if (!IsArchivePath(options.archive_path)) {
    throw std::runtime_error("archive path must use the .zip extension");
  }

  const auto package_status = fs::symlink_status(options.package_dir);
  if (fs::is_symlink(package_status)) {
    throw std::runtime_error("package directory cannot be a symlink: " +
                             options.package_dir.string());
  }
  if (!fs::exists(package_status) || !fs::is_directory(package_status)) {
    throw std::runtime_error("package directory does not exist: " +
                             options.package_dir.string());
  }

  if (options.package_dir.lexically_normal() == options.archive_path.lexically_normal()) {
    throw std::runtime_error("package directory and archive path must differ");
  }
  const auto normalized_package_dir =
      fs::absolute(options.package_dir).lexically_normal();
  const auto normalized_archive_path =
      fs::absolute(options.archive_path).lexically_normal();
  if (IsWithinBase(normalized_package_dir, normalized_archive_path)) {
    throw std::runtime_error("archive path cannot be inside the package directory: " +
                             options.archive_path.string());
  }

  const auto entries = CollectSourceEntries(options.package_dir);
  const auto has_manifest = std::any_of(
      entries.begin(), entries.end(),
      [](const SourceEntry& entry) { return entry.path == kManifestPath; });
  if (!has_manifest) {
    throw std::runtime_error("package directory must include META/manifest.json");
  }

  if (fs::exists(options.archive_path)) {
    if (!fs::is_regular_file(fs::symlink_status(options.archive_path))) {
      throw std::runtime_error("archive path exists but is not a regular file: " +
                               options.archive_path.string());
    }
    if (!options.force) {
      throw std::runtime_error("archive path already exists: " +
                               options.archive_path.string());
    }
    fs::remove(options.archive_path);
  }

  if (const auto parent = options.archive_path.parent_path(); !parent.empty()) {
    fs::create_directories(parent);
  }

  std::ofstream archive(options.archive_path, std::ios::binary | std::ios::trunc);
  if (!archive) {
    throw std::runtime_error("failed to open file for write: " +
                             options.archive_path.string());
  }

  std::vector<ArchiveRecord> written_entries;
  written_entries.reserve(entries.size());

  for (const auto& entry : entries) {
    const auto header_offset = static_cast<std::streamoff>(archive.tellp());
    if (header_offset < 0) {
      throw std::runtime_error("failed to query archive position");
    }

    WriteLocalFileHeader(archive, entry);
    EnsureStreamOk(archive, "failed to write ZIP local header");

    const FileWriteStats stats = WriteDeflatedFileData(entry.source_path, archive);
    const auto data_end = archive.tellp();
    if (data_end == std::streampos(-1)) {
      throw std::runtime_error("failed to query archive position");
    }

    PatchLocalFileHeader(archive, header_offset, stats);
    archive.seekp(data_end);
    EnsureStreamOk(archive, "failed to restore archive position");

    ArchiveRecord written_entry;
    written_entry.path = entry.path;
    written_entry.executable = entry.executable;
    written_entry.compression_method = kDeflatedCompressionMethod;
    written_entry.general_purpose_bit_flag = kGeneralPurposeUtf8;
    written_entry.crc32 = stats.crc32;
    written_entry.compressed_size = stats.compressed_size;
    written_entry.uncompressed_size = stats.uncompressed_size;
    written_entry.local_header_offset = static_cast<std::uint64_t>(header_offset);
    written_entries.push_back(std::move(written_entry));
  }

  const auto central_directory_offset_pos = archive.tellp();
  if (central_directory_offset_pos == std::streampos(-1)) {
    throw std::runtime_error("failed to query archive position");
  }
  const auto central_directory_offset =
      static_cast<std::uint64_t>(static_cast<std::streamoff>(central_directory_offset_pos));
  if (central_directory_offset > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("ZIP64 archives are not supported");
  }

  for (const auto& entry : written_entries) {
    WriteCentralDirectoryEntry(archive, entry);
    EnsureStreamOk(archive, "failed to write ZIP central directory");
  }

  const auto central_directory_end_pos = archive.tellp();
  if (central_directory_end_pos == std::streampos(-1)) {
    throw std::runtime_error("failed to query archive position");
  }
  const auto central_directory_end =
      static_cast<std::uint64_t>(static_cast<std::streamoff>(central_directory_end_pos));
  if (central_directory_end < central_directory_offset) {
    throw std::runtime_error("failed to finalize ZIP central directory");
  }
  const std::uint64_t central_directory_size =
      central_directory_end - central_directory_offset;
  if (central_directory_size > std::numeric_limits<std::uint32_t>::max() ||
      written_entries.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("ZIP64 archives are not supported");
  }

  WriteU32(archive, kEndOfCentralDirectorySignature);
  WriteU16(archive, 0U);
  WriteU16(archive, 0U);
  WriteU16(archive, static_cast<std::uint16_t>(written_entries.size()));
  WriteU16(archive, static_cast<std::uint16_t>(written_entries.size()));
  WriteU32(archive, static_cast<std::uint32_t>(central_directory_size));
  WriteU32(archive, static_cast<std::uint32_t>(central_directory_offset));
  WriteU16(archive, 0U);
  EnsureStreamOk(archive, "failed to write ZIP EOCD");
}

void ExtractArchive(const ExtractArchiveOptions& options) {
  if (options.archive_path.empty()) {
    throw std::runtime_error("archive path is empty");
  }
  if (options.output_dir.empty()) {
    throw std::runtime_error("output directory is empty");
  }
  if (!IsArchivePath(options.archive_path)) {
    throw std::runtime_error("archive path must use the .zip extension");
  }

  const auto records = ParseCentralDirectory(options.archive_path);

  if (fs::exists(options.output_dir)) {
    if (!options.force) {
      throw std::runtime_error("output directory already exists: " +
                               options.output_dir.string());
    }
    fs::remove_all(options.output_dir);
  }
  fs::create_directories(options.output_dir);

  for (const auto& record : records) {
    ExtractArchiveEntry(options.archive_path, record, options.output_dir);
  }
}

std::vector<ArchiveEntry> ListArchiveEntries(const fs::path& archive_path) {
  if (archive_path.empty()) {
    throw std::runtime_error("archive path is empty");
  }
  if (!IsArchivePath(archive_path)) {
    throw std::runtime_error("archive path must use the .zip extension");
  }

  const auto records = ParseCentralDirectory(archive_path);
  std::vector<ArchiveEntry> entries;
  entries.reserve(records.size());
  for (const auto& record : records) {
    ArchiveEntry entry;
    entry.path = record.path;
    entry.executable = record.executable;
    entry.compressed_size = record.compressed_size;
    entry.uncompressed_size = record.uncompressed_size;
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::vector<std::uint8_t> ReadArchiveFile(const fs::path& archive_path,
                                          std::string_view path) {
  if (archive_path.empty()) {
    throw std::runtime_error("archive path is empty");
  }
  if (!IsArchivePath(archive_path)) {
    throw std::runtime_error("archive path must use the .zip extension");
  }
  if (!IsSafeRelativeArchivePath(path)) {
    throw std::runtime_error("archive entry path is not safe: " + std::string(path));
  }

  const auto records = ParseCentralDirectory(archive_path);
  const auto record = FindArchiveRecord(records, path);
  return ReadArchiveEntryBytes(archive_path, record);
}

} // namespace zfleet::package
