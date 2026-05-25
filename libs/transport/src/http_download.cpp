#include "zfleet/transport/http_download.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <stdexcept>
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
  http::response<http::vector_body<std::uint8_t>> inbound;
  http::read(stream, buffer, inbound);
  return HttpDownloadResponse{
      .status = static_cast<int>(inbound.result_int()),
      .body = std::move(inbound.body()),
  };
}

}  // namespace zfleet::transport
