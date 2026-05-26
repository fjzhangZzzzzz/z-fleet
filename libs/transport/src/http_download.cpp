#include "zfleet/transport/http_download.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <span>
#include <string_view>

namespace zfleet::transport {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct HttpEndpoint {
  std::string host;
  std::string port;
  std::string target;
};

HttpEndpoint ParseHttpUrl(std::string_view url) {
  constexpr std::string_view kPrefix = "http://";
  if (!url.starts_with(kPrefix)) {
    throw std::invalid_argument("URL must start with http://");
  }
  url.remove_prefix(kPrefix.size());
  const auto slash = url.find('/');
  const auto authority = url.substr(0, slash);
  if (authority.empty() || slash == std::string_view::npos) {
    throw std::invalid_argument("URL must include host and path");
  }
  const auto delimiter = authority.rfind(':');
  return HttpEndpoint{
      .host = std::string(authority.substr(0, delimiter)),
      .port = delimiter == std::string_view::npos
                  ? "80"
                  : std::string(authority.substr(delimiter + 1)),
      .target = std::string(url.substr(slash)),
  };
}

}  // namespace

HttpDownloadResponse DownloadHttp(const HttpDownloadRequest& request) {
  HttpDownloadResponse response;
  const auto status = DownloadHttpStream(
      request, [&response](std::span<const std::uint8_t> chunk) {
        response.body.insert(response.body.end(), chunk.begin(), chunk.end());
      });
  response.status = status;
  return response;
}

int DownloadHttpStream(const HttpDownloadRequest& request,
                       const HttpDownloadChunkCallback& on_chunk) {
  const auto endpoint = ParseHttpUrl(request.url);
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  beast::tcp_stream stream(io_context);
  stream.connect(resolver.resolve(endpoint.host, endpoint.port));

  http::request<http::empty_body> outbound{http::verb::get, endpoint.target,
                                           11};
  outbound.set(http::field::host, endpoint.host);
  outbound.set(http::field::user_agent, request.user_agent);
  http::write(stream, outbound);

  beast::flat_buffer buffer;
  http::response_parser<http::buffer_body> parser;
  parser.eager(false);
  http::read_header(stream, buffer, parser);

  std::array<std::uint8_t, 64 * 1024> chunk_buffer{};
  boost::system::error_code error;
  while (!parser.is_done()) {
    parser.get().body().data = chunk_buffer.data();
    parser.get().body().size = chunk_buffer.size();
    http::read(stream, buffer, parser, error);
    if (error == http::error::need_buffer) {
      const auto bytes = chunk_buffer.size() - parser.get().body().size;
      if (bytes > 0) {
        on_chunk(std::span<const std::uint8_t>{chunk_buffer.data(), bytes});
      }
      continue;
    }
    if (error) {
      throw boost::system::system_error(error);
    }
    const auto bytes = chunk_buffer.size() - parser.get().body().size;
    if (bytes > 0) {
      on_chunk(std::span<const std::uint8_t>{chunk_buffer.data(), bytes});
    }
  }
  return static_cast<int>(parser.get().result_int());
}

int DownloadHttpToFile(const HttpDownloadRequest& request,
                       const std::filesystem::path& file_path) {
  std::filesystem::create_directories(file_path.parent_path());
  std::ofstream stream(file_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("failed to open download target file");
  }
  return DownloadHttpStream(
      request, [&stream](std::span<const std::uint8_t> chunk) {
        stream.write(reinterpret_cast<const char*>(chunk.data()),
                     static_cast<std::streamsize>(chunk.size()));
        if (!stream) {
          throw std::runtime_error("failed to write downloaded file");
        }
      });
}

}  // namespace zfleet::transport
