#include "http2_test_util.h"

#include "zfleet/transport/frame_codec.h"

#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <istream>
#include <iterator>
#include <stdexcept>

namespace zfleet::test {

namespace {

ssize_t ClientSendCallback(nghttp2_session* /*session*/,
                           const std::uint8_t* data, std::size_t length,
                           int /*flags*/, void* user_data) {
  auto* context = static_cast<ClientContext*>(user_data);
  boost::asio::write(context->socket, boost::asio::buffer(data, length));
  return static_cast<ssize_t>(length);
}

ssize_t ClientReadCallback(nghttp2_session* /*session*/,
                           std::int32_t /*stream_id*/, std::uint8_t* buffer,
                           std::size_t length, std::uint32_t* data_flags,
                           nghttp2_data_source* source, void* /*user_data*/) {
  auto* body = static_cast<ClientBody*>(source->ptr);
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

int ClientHeaderCallback(nghttp2_session* /*session*/,
                         const nghttp2_frame* frame, const std::uint8_t* name,
                         std::size_t name_length, const std::uint8_t* value,
                         std::size_t value_length, std::uint8_t /*flags*/,
                         void* user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }

  const std::string_view header_name(reinterpret_cast<const char*>(name),
                                     name_length);
  if (header_name == ":status") {
    auto* context = static_cast<ClientContext*>(user_data);
    context->response_status.assign(reinterpret_cast<const char*>(value),
                                    value_length);
  }
  return 0;
}

int ClientFrameRecvCallback(nghttp2_session* /*session*/,
                            const nghttp2_frame* frame, void* user_data) {
  if (((frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_RESPONSE) ||
       frame->hd.type == NGHTTP2_DATA) &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    auto* context = static_cast<ClientContext*>(user_data);
    context->response_done = true;
  }
  return 0;
}

int ClientDataChunkRecvCallback(nghttp2_session* /*session*/,
                                std::uint8_t /*flags*/,
                                std::int32_t /*stream_id*/,
                                const std::uint8_t* data, std::size_t length,
                                void* user_data) {
  auto* context = static_cast<ClientContext*>(user_data);
  context->response_body.insert(context->response_body.end(), data,
                                data + length);
  return 0;
}

HttpResponse ReadHttpResponse(tcp::socket* socket) {
  boost::asio::streambuf response;
  boost::system::error_code ec;
  boost::asio::read(*socket, response, ec);
  if (ec != boost::asio::error::eof) {
    throw std::runtime_error("failed to read http response");
  }

  std::istream stream(&response);
  std::string text((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  const auto header_end = text.find("\r\n\r\n");
  const auto status_end = text.find("\r\n");
  if (header_end == std::string::npos || status_end == std::string::npos) {
    throw std::runtime_error("malformed http response");
  }
  HttpResponse parsed;
  parsed.status = std::stoi(text.substr(9, 3));
  parsed.headers = text.substr(0, header_end);
  parsed.body = text.substr(header_end + 4);
  return parsed;
}

}  // namespace

void CheckNghttp2(int rv, std::string_view operation) {
  if (rv < 0) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + nghttp2_strerror(rv));
  }
}

ClientCallbacks::ClientCallbacks() {
  CheckNghttp2(nghttp2_session_callbacks_new(&callbacks),
               "create client callbacks");
  nghttp2_session_callbacks_set_send_callback(callbacks, ClientSendCallback);
  nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                   ClientHeaderCallback);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, ClientDataChunkRecvCallback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       ClientFrameRecvCallback);
}

ClientCallbacks::~ClientCallbacks() { nghttp2_session_callbacks_del(callbacks); }

ClientSession::ClientSession(const ClientCallbacks& callbacks,
                             ClientContext* context) {
  CheckNghttp2(
      nghttp2_session_client_new(&session, callbacks.callbacks, context),
      "create client session");
  CheckNghttp2(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0),
               "submit client settings");
}

ClientSession::~ClientSession() { nghttp2_session_del(session); }

HttpResponse SendHttpRequest(std::uint16_t port, const std::string& request) {
  asio::io_context io_context;
  tcp::socket socket(io_context);
  socket.connect({asio::ip::make_address("127.0.0.1"), port});
  asio::write(socket, asio::buffer(request));
  return ReadHttpResponse(&socket);
}

std::vector<std::uint8_t> EncodeEventFrame(
    const zfleet::protocol::v1::AgentEvent& event) {
  std::string bytes;
  if (!event.SerializeToString(&bytes)) {
    throw std::runtime_error("failed to serialize protobuf message");
  }
  return zfleet::transport::EncodeFrame(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

}  // namespace zfleet::test
