#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace zfleet::transport {

struct HttpDownloadRequest {
  std::string url;
  std::string user_agent = "zfleet";
};

struct HttpDownloadResponse {
  int status = 0;
  std::vector<std::uint8_t> body;
};

using HttpDownloadChunkCallback =
    std::function<void(std::span<const std::uint8_t> chunk)>;

HttpDownloadResponse DownloadHttp(const HttpDownloadRequest& request);
int DownloadHttpStream(const HttpDownloadRequest& request,
                       const HttpDownloadChunkCallback& on_chunk);
int DownloadHttpToFile(const HttpDownloadRequest& request,
                       const std::filesystem::path& file_path);

}  // namespace zfleet::transport
