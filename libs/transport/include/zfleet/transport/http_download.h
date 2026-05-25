#pragma once

#include <cstdint>
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

HttpDownloadResponse DownloadHttp(const HttpDownloadRequest& request);

}  // namespace zfleet::transport
