#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nghttp2/nghttp2.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "zfleet/protocol/v1/agent_control.pb.h"

namespace zfleet::test {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

struct HttpResponse {
  int status = 0;
  std::string headers;
  std::string body;
};

struct ClientBody {
  std::vector<std::uint8_t> bytes;
  std::size_t offset = 0;
};

struct ClientContext {
  tcp::socket socket;
  std::string response_status;
  std::vector<std::uint8_t> response_body;
  bool response_done = false;

  explicit ClientContext(asio::io_context& io_context) : socket(io_context) {}
};

struct ClientCallbacks {
  nghttp2_session_callbacks* callbacks = nullptr;

  ClientCallbacks();
  ~ClientCallbacks();

  ClientCallbacks(const ClientCallbacks&) = delete;
  ClientCallbacks& operator=(const ClientCallbacks&) = delete;
};

struct ClientSession {
  nghttp2_session* session = nullptr;

  ClientSession(const ClientCallbacks& callbacks, ClientContext* context);
  ~ClientSession();

  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;
};

void CheckNghttp2(int rv, std::string_view operation);
HttpResponse SendHttpRequest(std::uint16_t port, const std::string& request);
std::vector<std::uint8_t> EncodeEventFrame(
    const zfleet::protocol::v1::AgentEvent& event);

}  // namespace zfleet::test
