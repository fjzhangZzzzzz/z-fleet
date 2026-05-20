#include <zfleet/crypto/sha256.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zfleet::crypto {
namespace {

namespace fs = std::filesystem;

class Sha256Context {
 public:
  Sha256Context() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
    if (!context_) {
      throw std::runtime_error("failed to allocate sha256 context");
    }
    if (EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("failed to initialize sha256");
    }
  }

  void Update(const void* data, std::size_t size) {
    if (size == 0) {
      return;
    }
    if (EVP_DigestUpdate(context_.get(), data, size) != 1) {
      throw std::runtime_error("failed to update sha256");
    }
  }

  std::string Finish() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    if (EVP_DigestFinal_ex(context_.get(), digest.data(), &digest_length) !=
        1) {
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

 private:
  using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  MdCtxPtr context_;
};

}  // namespace

std::string Sha256BytesHex(std::string_view data) {
  Sha256Context context;
  context.Update(data.data(), data.size());
  return context.Finish();
}

std::string Sha256FileHex(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open file for sha256: " +
                             path.string());
  }

  Sha256Context context;
  std::array<char, 8192> buffer{};
  while (stream.good()) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytes_read = stream.gcount();
    if (bytes_read <= 0) {
      break;
    }
    context.Update(buffer.data(), static_cast<std::size_t>(bytes_read));
  }
  return context.Finish();
}

bool IsLowerHexSha256(std::string_view value) noexcept {
  if (value.size() != 64) {
    return false;
  }

  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
  });
}

}  // namespace zfleet::crypto
