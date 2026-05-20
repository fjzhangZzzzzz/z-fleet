#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace zfleet::crypto {

std::string Sha256BytesHex(std::string_view data);
std::string Sha256FileHex(const std::filesystem::path& path);
bool IsLowerHexSha256(std::string_view value) noexcept;

}  // namespace zfleet::crypto
