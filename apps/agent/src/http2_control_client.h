#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <chrono>
#include <map>
#include <nghttp2/nghttp2.h>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zfleet::agent {

struct Http2Response {
  std::string status;
  std::vector<std::uint8_t> body;
};

class Http2ControlClient {
 public:
  explicit Http2ControlClient(std::string control_url);
  ~Http2ControlClient();

  Http2ControlClient(const Http2ControlClient&) = delete;
  Http2ControlClient& operator=(const Http2ControlClient&) = delete;

  void Connect();
  void Close() noexcept;

  Http2Response PostEvents(std::span<const std::uint8_t> body);
  std::string StartCommandStream(std::string_view correlation_id);
  void PumpFor(std::chrono::milliseconds timeout);
  std::vector<std::uint8_t> DrainCommandBytes();
  bool command_stream_open() const noexcept;
  std::optional<std::uint32_t> command_stream_error_code() const noexcept;

  bool connected() const noexcept;

 private:
  struct ParsedEndpoint {
    std::string host;
    std::string authority;
    std::uint16_t port = 0;
  };

  struct RequestBody {
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
  };

  struct ResponseState {
    std::string status;
    std::vector<std::uint8_t> body;
    std::optional<std::uint32_t> close_error_code;
    bool headers_received = false;
    bool done = false;
  };

  struct Context {
    boost::asio::ip::tcp::socket socket;
    std::optional<RequestBody> request_body;
    std::map<std::int32_t, ResponseState> responses;
    std::optional<std::int32_t> command_stream_id;

    explicit Context(boost::asio::io_context& io_context)
        : socket(io_context) {}
  };

  struct SessionCallbacks {
    nghttp2_session_callbacks* callbacks = nullptr;

    SessionCallbacks();
    ~SessionCallbacks();
  };

  struct Session {
    nghttp2_session* session = nullptr;

    Session(const SessionCallbacks& callbacks, Context* context);
    ~Session();
  };

  ParsedEndpoint ParseControlUrl(std::string_view control_url) const;
  void EnsureConnected();
  Http2Response SubmitRequest(std::string method,
                              std::string path,
                              std::vector<std::pair<std::string, std::string>>
                                  extra_headers,
                              std::vector<std::uint8_t> body);
  void Flush();
  void PumpUntilResponseDone(std::int32_t stream_id);
  void PumpUntilHeaders(std::int32_t stream_id);
  void ThrowIfStreamReset(const ResponseState& response,
                          std::string_view operation) const;
  bool PumpOnce();

  static ssize_t SendCallback(nghttp2_session* session,
                              const std::uint8_t* data,
                              std::size_t length,
                              int flags,
                              void* user_data);
  static ssize_t ReadCallback(nghttp2_session* session,
                              std::int32_t stream_id,
                              std::uint8_t* buffer,
                              std::size_t length,
                              std::uint32_t* data_flags,
                              nghttp2_data_source* source,
                              void* user_data);
  static int OnHeaderCallback(nghttp2_session* session,
                              const nghttp2_frame* frame,
                              const std::uint8_t* name,
                              std::size_t name_length,
                              const std::uint8_t* value,
                              std::size_t value_length,
                              std::uint8_t flags,
                              void* user_data);
  static int OnDataChunkRecvCallback(nghttp2_session* session,
                                     std::uint8_t flags,
                                     std::int32_t stream_id,
                                     const std::uint8_t* data,
                                     std::size_t length,
                                     void* user_data);
  static int OnFrameRecvCallback(nghttp2_session* session,
                                 const nghttp2_frame* frame,
                                 void* user_data);
  static int OnStreamCloseCallback(nghttp2_session* session,
                                   std::int32_t stream_id,
                                   std::uint32_t error_code,
                                   void* user_data);
  static void CheckNghttp2(int rv, std::string_view operation);
  static nghttp2_nv MakeHeader(std::string_view name, std::string_view value);

  boost::asio::io_context io_context_;
  ParsedEndpoint endpoint_;
  Context context_;
  SessionCallbacks callbacks_;
  std::optional<Session> session_;
};

} // namespace zfleet::agent
