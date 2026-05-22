#include "http2_control_client.h"

#include "zfleet/core/log.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>
#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zfleet::agent {
namespace {

constexpr std::size_t kReadBufferBytes = 16 * 1024;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

std::string StripHttpScheme(std::string_view control_url) {
  constexpr std::string_view kHttpPrefix = "http://";
  if (control_url.rfind(kHttpPrefix, 0) == 0) {
    return std::string(control_url.substr(kHttpPrefix.size()));
  }

  constexpr std::string_view kHttpsPrefix = "https://";
  if (control_url.rfind(kHttpsPrefix, 0) == 0) {
    throw std::invalid_argument("https control url is not supported yet");
  }

  return std::string(control_url);
}

} // namespace

void Http2ControlClient::CheckNghttp2(int rv, std::string_view operation) {
  if (rv < 0) {
    throw std::runtime_error(std::string(operation) + " failed: " +
                             nghttp2_strerror(rv));
  }
}

nghttp2_nv Http2ControlClient::MakeHeader(std::string_view name,
                                          std::string_view value) {
  return nghttp2_nv{
      reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
      reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
      name.size(),
      value.size(),
      NGHTTP2_NV_FLAG_NONE,
  };
}

Http2ControlClient::SessionCallbacks::SessionCallbacks() {
  CheckNghttp2(nghttp2_session_callbacks_new(&callbacks),
               "create nghttp2 callbacks");
  nghttp2_session_callbacks_set_send_callback(callbacks, SendCallback);
  nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                   OnHeaderCallback);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, OnDataChunkRecvCallback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       OnFrameRecvCallback);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks,
                                                         OnStreamCloseCallback);
}

Http2ControlClient::SessionCallbacks::~SessionCallbacks() {
  if (callbacks != nullptr) {
    nghttp2_session_callbacks_del(callbacks);
  }
}

Http2ControlClient::Session::Session(const SessionCallbacks& callbacks,
                                     Context* context) {
  CheckNghttp2(nghttp2_session_client_new(&session, callbacks.callbacks,
                                          context),
               "create nghttp2 client session");
  CheckNghttp2(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0),
               "submit client settings");
}

Http2ControlClient::Session::~Session() {
  if (session != nullptr) {
    nghttp2_session_del(session);
  }
}

Http2ControlClient::Http2ControlClient(std::string control_url)
    : endpoint_(ParseControlUrl(control_url)),
      context_(io_context_) {}

Http2ControlClient::~Http2ControlClient() {
  Close();
}

Http2ControlClient::ParsedEndpoint Http2ControlClient::ParseControlUrl(
    std::string_view control_url) const {
  const auto stripped = StripHttpScheme(control_url);
  if (stripped.empty()) {
    throw std::invalid_argument("control url must not be empty");
  }

  const auto path_pos = stripped.find('/');
  const auto authority = stripped.substr(0, path_pos);
  if (authority.empty()) {
    throw std::invalid_argument("control url must include host and port");
  }

  std::string host;
  std::string port_text;
  if (authority.front() == '[') {
    const auto closing = authority.find(']');
    Check(closing != std::string::npos && closing + 2 <= authority.size() &&
              authority[closing + 1] == ':',
          "control url must be host:port");
    host = std::string(authority.substr(1, closing - 1));
    port_text = std::string(authority.substr(closing + 2));
  } else {
    const auto delimiter = authority.rfind(':');
    Check(delimiter != std::string::npos && delimiter + 1 < authority.size(),
          "control url must be host:port");
    host = std::string(authority.substr(0, delimiter));
    port_text = std::string(authority.substr(delimiter + 1));
  }

  const auto port_value = static_cast<unsigned long>(std::stoul(port_text));
  Check(port_value > 0 && port_value <= 65535,
        "control url port is out of range");

  std::string authority_text;
  if (host.find(':') != std::string::npos) {
    authority_text = "[" + host + "]:" + std::to_string(port_value);
  } else {
    authority_text = host + ":" + std::to_string(port_value);
  }

  return ParsedEndpoint{
      .host = std::move(host),
      .authority = std::move(authority_text),
      .port = static_cast<std::uint16_t>(port_value),
  };
}

void Http2ControlClient::Connect() {
  if (connected()) {
    return;
  }

  auto endpoints = boost::asio::ip::tcp::resolver(io_context_).resolve(
      endpoint_.host, std::to_string(endpoint_.port));
  boost::asio::connect(context_.socket, endpoints);
  context_.socket.non_blocking(true);
  session_.emplace(callbacks_, &context_);
  Flush();
}

void Http2ControlClient::Close() noexcept {
  session_.reset();
  boost::system::error_code ec;
  context_.socket.close(ec);
  context_.request_body.reset();
  context_.responses.clear();
  context_.command_stream_id.reset();
}

bool Http2ControlClient::connected() const noexcept {
  return context_.socket.is_open() && session_.has_value();
}

Http2Response Http2ControlClient::PostEvents(std::span<const std::uint8_t> body) {
  return SubmitRequest(
      "POST", "/v1/control/events",
      {{"content-type", "application/x-protobuf"}},
      std::vector<std::uint8_t>(body.begin(), body.end()));
}

std::string Http2ControlClient::StartCommandStream(
    std::string_view correlation_id) {
  EnsureConnected();
  if (context_.command_stream_id.has_value()) {
    const auto response = context_.responses.find(*context_.command_stream_id);
    if (response != context_.responses.end() && !response->second.done) {
      return response->second.status;
    }
  }

  std::vector<std::pair<std::string, std::string>> extra_headers{
      {"accept", "application/x-protobuf"},
      {"x-zfleet-correlation-id", std::string(correlation_id)},
  };

  std::vector<nghttp2_nv> headers;
  headers.reserve(4 + extra_headers.size());
  headers.push_back(MakeHeader(":method", "GET"));
  headers.push_back(MakeHeader(":scheme", "http"));
  headers.push_back(MakeHeader(":authority", endpoint_.authority));
  headers.push_back(MakeHeader(":path", "/v1/control/commands"));
  for (const auto& [name, value] : extra_headers) {
    headers.push_back(MakeHeader(name, value));
  }

  const auto stream_id = nghttp2_submit_request(
      session_->session, nullptr, headers.data(), headers.size(), nullptr,
      nullptr);
  CheckNghttp2(stream_id, "submit command stream request");
  context_.responses.insert_or_assign(stream_id, ResponseState{});
  context_.command_stream_id = stream_id;

  Flush();
  PumpUntilHeaders(stream_id);
  return context_.responses.at(stream_id).status;
}

void Http2ControlClient::PumpFor(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const bool received = PumpOnce();
    Flush();
    if (!received) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

std::vector<std::uint8_t> Http2ControlClient::DrainCommandBytes() {
  if (!context_.command_stream_id.has_value()) {
    return {};
  }
  auto response = context_.responses.find(*context_.command_stream_id);
  if (response == context_.responses.end()) {
    return {};
  }
  std::vector<std::uint8_t> bytes = std::move(response->second.body);
  response->second.body.clear();
  return bytes;
}

bool Http2ControlClient::command_stream_open() const noexcept {
  if (!context_.command_stream_id.has_value()) {
    return false;
  }
  const auto response = context_.responses.find(*context_.command_stream_id);
  return response != context_.responses.end() && !response->second.done;
}

std::optional<std::uint32_t>
Http2ControlClient::command_stream_error_code() const noexcept {
  if (!context_.command_stream_id.has_value()) {
    return std::nullopt;
  }
  const auto response = context_.responses.find(*context_.command_stream_id);
  if (response == context_.responses.end()) {
    return std::nullopt;
  }
  return response->second.close_error_code;
}

Http2Response Http2ControlClient::SubmitRequest(
    std::string method,
    std::string path,
    std::vector<std::pair<std::string, std::string>> extra_headers,
    std::vector<std::uint8_t> body) {
  EnsureConnected();

  context_.request_body = RequestBody{.bytes = std::move(body), .offset = 0};

  std::vector<nghttp2_nv> headers;
  headers.reserve(4 + extra_headers.size());
  headers.push_back(MakeHeader(":method", method));
  headers.push_back(MakeHeader(":scheme", "http"));
  headers.push_back(MakeHeader(":authority", endpoint_.authority));
  headers.push_back(MakeHeader(":path", path));
  for (const auto& [name, value] : extra_headers) {
    headers.push_back(MakeHeader(name, value));
  }

  nghttp2_data_provider provider;
  nghttp2_data_provider* provider_ptr = nullptr;
  if (!context_.request_body->bytes.empty()) {
    provider.source.ptr = &*context_.request_body;
    provider.read_callback = ReadCallback;
    provider_ptr = &provider;
  }

  const auto stream_id = nghttp2_submit_request(
      session_->session, nullptr, headers.data(), headers.size(), provider_ptr,
      nullptr);
  CheckNghttp2(stream_id, "submit http2 request");
  context_.responses.insert_or_assign(stream_id, ResponseState{});

  Flush();
  PumpUntilResponseDone(stream_id);

  auto response_state = context_.responses.find(stream_id);
  Check(response_state != context_.responses.end(),
        "http2 response state missing");

  Http2Response response{
      .status = std::move(response_state->second.status),
      .body = std::move(response_state->second.body),
  };
  context_.responses.erase(response_state);
  context_.request_body.reset();
  return response;
}

void Http2ControlClient::EnsureConnected() {
  if (!connected()) {
    Connect();
  }
}

void Http2ControlClient::Flush() {
  CheckNghttp2(nghttp2_session_send(session_->session), "send http2 bytes");
}

void Http2ControlClient::PumpUntilResponseDone(std::int32_t stream_id) {
  while (true) {
    const auto response = context_.responses.find(stream_id);
    if (response != context_.responses.end() && response->second.done) {
      ThrowIfStreamReset(response->second, "http2 response stream");
      if (!response->second.headers_received) {
        throw std::runtime_error("http2 response stream closed before headers");
      }
      return;
    }
    PumpOnce();
    Flush();
  }
}

void Http2ControlClient::PumpUntilHeaders(std::int32_t stream_id) {
  while (true) {
    const auto response = context_.responses.find(stream_id);
    if (response != context_.responses.end() &&
        (response->second.headers_received || response->second.done)) {
      ThrowIfStreamReset(response->second, "command stream");
      if (response->second.done && !response->second.headers_received) {
        throw std::runtime_error("command stream closed before headers");
      }
      return;
    }
    PumpOnce();
    Flush();
  }
}

void Http2ControlClient::ThrowIfStreamReset(
    const ResponseState& response,
    std::string_view operation) const {
  if (response.close_error_code.has_value() &&
      *response.close_error_code != NGHTTP2_NO_ERROR) {
    throw std::runtime_error(std::string(operation) + " reset: " +
                             nghttp2_http2_strerror(
                                 *response.close_error_code));
  }
}

bool Http2ControlClient::PumpOnce() {
  std::array<std::uint8_t, kReadBufferBytes> buffer{};
  boost::system::error_code ec;
  const auto bytes_read =
      context_.socket.read_some(boost::asio::buffer(buffer), ec);
  if (ec == boost::asio::error::would_block ||
      ec == boost::asio::error::try_again) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return false;
  }
  if (ec == boost::asio::error::eof) {
    throw std::runtime_error("http2 control connection closed");
  }
  if (ec) {
    throw boost::system::system_error(ec);
  }
  const auto rv = nghttp2_session_mem_recv(session_->session, buffer.data(),
                                           bytes_read);
  CheckNghttp2(static_cast<int>(rv), "receive http2 bytes");
  return bytes_read > 0;
}

ssize_t Http2ControlClient::SendCallback(nghttp2_session* /*session*/,
                                         const std::uint8_t* data,
                                         std::size_t length,
                                         int /*flags*/,
                                         void* user_data) {
  auto* context = static_cast<Context*>(user_data);
  boost::system::error_code ec;
  boost::asio::write(context->socket, boost::asio::buffer(data, length),
                     boost::asio::transfer_all(), ec);
  if (ec) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  return static_cast<ssize_t>(length);
}

ssize_t Http2ControlClient::ReadCallback(nghttp2_session* /*session*/,
                                         std::int32_t /*stream_id*/,
                                         std::uint8_t* buffer,
                                         std::size_t length,
                                         std::uint32_t* data_flags,
                                         nghttp2_data_source* source,
                                         void* /*user_data*/) {
  auto* body = static_cast<RequestBody*>(source->ptr);
  const auto remaining = body->bytes.size() - body->offset;
  const auto bytes_to_copy = std::min(length, remaining);
  if (bytes_to_copy > 0) {
    std::memcpy(buffer, body->bytes.data() + body->offset, bytes_to_copy);
    body->offset += bytes_to_copy;
  }
  if (body->offset == body->bytes.size()) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  }
  return static_cast<ssize_t>(bytes_to_copy);
}

int Http2ControlClient::OnHeaderCallback(nghttp2_session* /*session*/,
                                         const nghttp2_frame* frame,
                                         const std::uint8_t* name,
                                         std::size_t name_length,
                                         const std::uint8_t* value,
                                         std::size_t value_length,
                                         std::uint8_t /*flags*/,
                                         void* user_data) {
  auto* context = static_cast<Context*>(user_data);
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }
  auto response = context->responses.find(frame->hd.stream_id);
  if (response == context->responses.end()) {
    return 0;
  }

  const std::string_view header_name(reinterpret_cast<const char*>(name),
                                     name_length);
  if (header_name == ":status") {
    response->second.status.assign(reinterpret_cast<const char*>(value),
                                   value_length);
    response->second.headers_received = true;
  }
  return 0;
}

int Http2ControlClient::OnDataChunkRecvCallback(
    nghttp2_session* /*session*/,
    std::uint8_t /*flags*/,
    std::int32_t stream_id,
    const std::uint8_t* data,
    std::size_t length,
    void* user_data) {
  auto* context = static_cast<Context*>(user_data);
  auto response = context->responses.find(stream_id);
  if (response == context->responses.end()) {
    return 0;
  }

  response->second.body.insert(response->second.body.end(), data,
                               data + length);
  return 0;
}

int Http2ControlClient::OnFrameRecvCallback(nghttp2_session* /*session*/,
                                            const nghttp2_frame* frame,
                                            void* user_data) {
  auto* context = static_cast<Context*>(user_data);
  auto response = context->responses.find(frame->hd.stream_id);
  if (response == context->responses.end()) {
    return 0;
  }

  if (frame->hd.type == NGHTTP2_HEADERS &&
      frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
    response->second.headers_received = true;
  }
  if (((frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_RESPONSE) ||
       frame->hd.type == NGHTTP2_DATA) &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    response->second.done = true;
  }
  return 0;
}

int Http2ControlClient::OnStreamCloseCallback(nghttp2_session* /*session*/,
                                              std::int32_t stream_id,
                                              std::uint32_t error_code,
                                              void* user_data) {
  auto* context = static_cast<Context*>(user_data);
  auto response = context->responses.find(stream_id);
  if (response != context->responses.end()) {
    response->second.close_error_code = error_code;
    response->second.done = true;
  }
  return 0;
}

} // namespace zfleet::agent
