#include "control_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "control_dispatcher.h"
#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/control_codec.h"
#include "zfleet/protocol/message.h"
#include "zfleet/transport/frame_codec.h"
#include "zfleet/transport/nghttp2_compat.h"

namespace zfleet::server {
namespace {

using tcp = boost::asio::ip::tcp;

constexpr std::size_t kReadBufferBytes = 16 * 1024;
#define ZFLEET_NGHTTP2_NV(NAME, VALUE)                             \
  nghttp2_nv {                                                     \
    reinterpret_cast<std::uint8_t*>(const_cast<char*>(NAME)),      \
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(VALUE)), \
        sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE  \
  }

tcp::endpoint ParseListenAddress(const std::string& listen_address) {
  const auto delimiter = listen_address.rfind(':');
  if (delimiter == std::string::npos) {
    throw std::invalid_argument("listen address must be host:port");
  }

  const auto host = listen_address.substr(0, delimiter);
  const auto port_text = listen_address.substr(delimiter + 1);
  const auto port_value = static_cast<unsigned short>(std::stoul(port_text));

  return tcp::endpoint(boost::asio::ip::make_address(host), port_value);
}

struct StreamState {
  std::string method;
  std::string path;
  std::string correlation_id;
  std::shared_ptr<ControlDispatcher> dispatcher;
  std::shared_ptr<std::mutex> dispatcher_mutex = std::make_shared<std::mutex>();
  std::vector<std::uint8_t> response_body;
  std::size_t response_offset = 0;
  std::size_t pending_event_chunks = 0;
  bool has_error = false;
  bool command_stream = false;
  bool command_headers_submitted = false;
  bool command_data_in_flight = false;
  bool command_claim_in_flight = false;
  bool command_refresh_pending = false;
  bool end_stream_received = false;
  bool response_submitted = false;
};

struct ConnectionContext
    : public std::enable_shared_from_this<ConnectionContext> {
  std::shared_ptr<tcp::socket> socket;
  ServerStore* store;
  const ControlService* service;
  ControlConnectionRegistry* registry;
  ControlWorkerPool* worker_pool;
  boost::asio::any_io_executor completion_executor;
  nghttp2_session* session = nullptr;
  std::deque<std::vector<std::uint8_t>> pending_writes;
  bool write_in_progress = false;
  std::function<void()> flush_output;
  std::function<void()> request_command_drain;
  std::string connection_id;
  std::map<std::int32_t, StreamState> streams;

  ConnectionContext(std::shared_ptr<tcp::socket> accepted_socket,
                    ServerStore* store_arg, const ControlService* service_arg,
                    ControlConnectionRegistry* registry_arg,
                    ControlWorkerPool* worker_pool_arg,
                    boost::asio::any_io_executor completion_executor_arg,
                    std::string connection_id_arg)
      : socket(std::move(accepted_socket)),
        store(store_arg),
        service(service_arg),
        registry(registry_arg),
        worker_pool(worker_pool_arg),
        completion_executor(std::move(completion_executor_arg)),
        connection_id(std::move(connection_id_arg)) {}
};

void CheckNghttp2(int rv, std::string_view operation) {
  if (rv < 0) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + nghttp2_strerror(rv));
  }
}

StreamState* FindStream(ConnectionContext* context, std::int32_t stream_id) {
  const auto stream = context->streams.find(stream_id);
  if (stream == context->streams.end()) {
    return nullptr;
  }
  return &stream->second;
}

std::vector<std::uint8_t> EncodeCommandFrame(const zfleet::protocol::Task& task,
                                             std::string_view correlation_id) {
  const auto payload = zfleet::protocol::EncodeServerCommandPayload(
      zfleet::protocol::ServerCommand{
          .protocol_version =
              std::string(zfleet::protocol::protocol_version()),
          .message_id = zfleet::core::GenerateUuid(),
          .correlation_id = std::string(correlation_id),
          .agent_id = task.agent_id,
          .occurred_at = zfleet::core::NowUtcRfc3339(),
          .payload = task,
      });
  return zfleet::transport::EncodeFrame(
      std::span<const std::uint8_t>{payload.data(), payload.size()});
}

void SubmitSimpleResponse(nghttp2_session* session, std::int32_t stream_id,
                          std::string_view status) {
  std::array<nghttp2_nv, 2> headers{
      ZFLEET_NGHTTP2_NV(":status", "200"),
      ZFLEET_NGHTTP2_NV("content-type", "application/x-protobuf"),
  };
  if (status == "400") {
    headers[0] = ZFLEET_NGHTTP2_NV(":status", "400");
  } else if (status == "404") {
    headers[0] = ZFLEET_NGHTTP2_NV(":status", "404");
  } else if (status == "500") {
    headers[0] = ZFLEET_NGHTTP2_NV(":status", "500");
  }
  CheckNghttp2(nghttp2_submit_response(session, stream_id, headers.data(),
                                       headers.size(), nullptr),
               "submit http2 response");
}

ssize_t ResponseReadCallback(nghttp2_session* /*session*/,
                             std::int32_t /*stream_id*/, std::uint8_t* buffer,
                             std::size_t length, std::uint32_t* data_flags,
                             nghttp2_data_source* source, void* /*user_data*/) {
  auto* stream = static_cast<StreamState*>(source->ptr);
  const auto remaining = stream->response_body.size() - stream->response_offset;
  const auto bytes_to_copy = std::min(length, remaining);
  if (bytes_to_copy > 0) {
    std::memcpy(buffer, stream->response_body.data() + stream->response_offset,
                bytes_to_copy);
    stream->response_offset += bytes_to_copy;
  }
  if (stream->response_offset == stream->response_body.size()) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  }
  return static_cast<ssize_t>(bytes_to_copy);
}

void SubmitCommandHeaders(nghttp2_session* session, std::int32_t stream_id,
                          StreamState* stream) {
  if (stream->command_headers_submitted) {
    return;
  }
  std::array<nghttp2_nv, 2> headers{
      ZFLEET_NGHTTP2_NV(":status", "200"),
      ZFLEET_NGHTTP2_NV("content-type", "application/x-protobuf"),
  };
  CheckNghttp2(
      nghttp2_submit_headers(session, NGHTTP2_FLAG_NONE, stream_id, nullptr,
                             headers.data(), headers.size(), nullptr),
      "submit command stream headers");
  stream->command_headers_submitted = true;
}

void SubmitCommandData(nghttp2_session* session, std::int32_t stream_id,
                       StreamState* stream) {
  std::array<nghttp2_nv, 2> headers{
      ZFLEET_NGHTTP2_NV(":status", "200"),
      ZFLEET_NGHTTP2_NV("content-type", "application/x-protobuf"),
  };
  nghttp2_data_provider provider;
  provider.source.ptr = stream;
  provider.read_callback = ResponseReadCallback;
  if (!stream->command_headers_submitted) {
    CheckNghttp2(nghttp2_submit_response(session, stream_id, headers.data(),
                                         headers.size(), &provider),
                 "submit command response");
    stream->command_headers_submitted = true;
  } else {
    CheckNghttp2(
        nghttp2_submit_data(session, NGHTTP2_FLAG_NONE, stream_id, &provider),
        "submit command data");
  }
  stream->command_data_in_flight = true;
}

bool IsWouldBlock(const boost::system::error_code& ec);

ssize_t SendCallback(nghttp2_session* /*session*/, const std::uint8_t* data,
                     std::size_t length, int /*flags*/, void* user_data) {
  auto* context = static_cast<ConnectionContext*>(user_data);
  context->pending_writes.emplace_back(data, data + length);
  return static_cast<ssize_t>(length);
}

int OnBeginHeadersCallback(nghttp2_session* /*session*/,
                           const nghttp2_frame* frame, void* user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  auto* context = static_cast<ConnectionContext*>(user_data);
  context->streams.insert_or_assign(frame->hd.stream_id, StreamState{});
  return 0;
}

int OnHeaderCallback(nghttp2_session* /*session*/, const nghttp2_frame* frame,
                     const std::uint8_t* name, std::size_t name_length,
                     const std::uint8_t* value, std::size_t value_length,
                     std::uint8_t /*flags*/, void* user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  auto* context = static_cast<ConnectionContext*>(user_data);
  auto* stream = FindStream(context, frame->hd.stream_id);
  if (stream == nullptr) {
    return 0;
  }

  const std::string_view header_name(reinterpret_cast<const char*>(name),
                                     name_length);
  const std::string_view header_value(reinterpret_cast<const char*>(value),
                                      value_length);
  if (header_name == ":method") {
    stream->method = header_value;
  } else if (header_name == ":path") {
    stream->path = header_value;
  } else if (header_name == "x-zfleet-correlation-id") {
    stream->correlation_id = header_value;
  }
  return 0;
}

void HandleCommandHeadersEnd(nghttp2_session* session,
                             const nghttp2_frame* frame,
                             ConnectionContext* context, StreamState* stream) {
  if (stream == nullptr || stream->method != "GET" ||
      stream->path != zfleet::transport::kControlCommandsPath) {
    SubmitSimpleResponse(session, frame->hd.stream_id, "400");
    return;
  }

  const auto connection =
      context->registry->FindByConnection(context->connection_id);
  if (!connection.has_value() || !connection->agent_id.has_value() ||
      connection->disconnected_at.has_value()) {
    SubmitSimpleResponse(session, frame->hd.stream_id, "404");
    return;
  }

  stream->command_stream = true;
  stream->command_refresh_pending = true;
  SubmitCommandHeaders(session, frame->hd.stream_id, stream);
  if (context->request_command_drain) {
    context->request_command_drain();
  }
}

void FinalizeSentCommandData(ConnectionContext* context) {
  for (auto& [_, stream] : context->streams) {
    if (stream.command_stream && stream.command_data_in_flight &&
        !stream.response_body.empty() &&
        stream.response_offset == stream.response_body.size()) {
      stream.response_body.clear();
      stream.response_offset = 0;
      stream.command_data_in_flight = false;
      stream.command_refresh_pending = true;
    }
  }
}

void TrySubmitEventResponse(nghttp2_session* session, std::int32_t stream_id,
                            StreamState* stream) {
  if (stream == nullptr || stream->response_submitted ||
      !stream->end_stream_received || stream->pending_event_chunks != 0) {
    return;
  }
  SubmitSimpleResponse(session, stream_id, !stream->has_error ? "200" : "400");
  stream->response_submitted = true;
}

int OnDataChunkRecvCallback(nghttp2_session* session, std::uint8_t /*flags*/,
                            std::int32_t stream_id, const std::uint8_t* data,
                            std::size_t length, void* user_data) {
  auto* context = static_cast<ConnectionContext*>(user_data);
  auto* stream = FindStream(context, stream_id);
  if (stream == nullptr) {
    return 0;
  }

  if (stream->method != "POST" ||
      stream->path != zfleet::transport::kControlEventsPath) {
    stream->has_error = true;
    return 0;
  }

  if (stream->dispatcher == nullptr) {
    stream->dispatcher = std::make_shared<ControlDispatcher>(
        context->service, context->registry, context->connection_id);
  }
  std::vector<std::uint8_t> bytes(data, data + length);
  ++stream->pending_event_chunks;
  const auto accepted = context->worker_pool->Submit(
      [dispatcher = stream->dispatcher,
       dispatcher_mutex = stream->dispatcher_mutex,
       bytes = std::move(bytes)]() {
        std::lock_guard lock(*dispatcher_mutex);
        return dispatcher->PushEventBytes(bytes);
      },
      context->completion_executor,
      [context = context->shared_from_this(), stream_id](
          std::exception_ptr error, std::vector<ControlEventResult> results) {
        auto* stream = FindStream(context.get(), stream_id);
        if (stream == nullptr || context->session == nullptr) {
          return;
        }
        if (stream->pending_event_chunks > 0) {
          --stream->pending_event_chunks;
        }
        if (error != nullptr) {
          stream->has_error = true;
        }
        for (const auto& result : results) {
          if (result.status != ControlEventStatus::kAccepted) {
            stream->has_error = true;
          }
        }
        TrySubmitEventResponse(context->session, stream_id, stream);
        CheckNghttp2(nghttp2_session_send(context->session),
                     "send http2 bytes");
        if (context->flush_output) {
          context->flush_output();
        }
      });
  if (!accepted) {
    if (stream->pending_event_chunks > 0) {
      --stream->pending_event_chunks;
    }
    stream->has_error = true;
    TrySubmitEventResponse(session, stream_id, stream);
    CheckNghttp2(nghttp2_session_send(session), "send http2 bytes");
    if (context->flush_output) {
      context->flush_output();
    }
  }
  return 0;
}

bool IsWouldBlock(const boost::system::error_code& ec) {
  return ec == boost::asio::error::would_block ||
         ec == boost::asio::error::try_again;
}

int OnFrameRecvCallback(nghttp2_session* session, const nghttp2_frame* frame,
                        void* user_data) {
  if (frame->hd.type == NGHTTP2_HEADERS &&
      frame->headers.cat == NGHTTP2_HCAT_REQUEST &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    auto* context = static_cast<ConnectionContext*>(user_data);
    auto* stream = FindStream(context, frame->hd.stream_id);
    if (stream != nullptr && stream->method == "GET") {
      HandleCommandHeadersEnd(session, frame, context, stream);
      return 0;
    }
    SubmitSimpleResponse(
        session, frame->hd.stream_id,
        stream != nullptr && !stream->has_error ? "200" : "400");
    return 0;
  }

  if (frame->hd.type == NGHTTP2_DATA &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    auto* context = static_cast<ConnectionContext*>(user_data);
    auto* stream = FindStream(context, frame->hd.stream_id);
    if (stream != nullptr) {
      stream->end_stream_received = true;
      TrySubmitEventResponse(session, frame->hd.stream_id, stream);
    }
  }
  return 0;
}

int OnStreamCloseCallback(nghttp2_session* /*session*/, std::int32_t stream_id,
                          std::uint32_t /*error_code*/, void* user_data) {
  auto* context = static_cast<ConnectionContext*>(user_data);
  context->streams.erase(stream_id);
  return 0;
}

struct SessionCallbacks {
  nghttp2_session_callbacks* callbacks = nullptr;

  SessionCallbacks() {
    CheckNghttp2(nghttp2_session_callbacks_new(&callbacks),
                 "create nghttp2 callbacks");
    nghttp2_session_callbacks_set_send_callback(callbacks, SendCallback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(
        callbacks, OnBeginHeadersCallback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                     OnHeaderCallback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, OnDataChunkRecvCallback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                         OnFrameRecvCallback);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks, OnStreamCloseCallback);
  }

  ~SessionCallbacks() { nghttp2_session_callbacks_del(callbacks); }
};

struct ServerSession {
  nghttp2_session* session = nullptr;

  ServerSession(const SessionCallbacks& callbacks, ConnectionContext* context) {
    CheckNghttp2(
        nghttp2_session_server_new(&session, callbacks.callbacks, context),
        "create nghttp2 server session");
    context->session = session;
    CheckNghttp2(
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0),
        "submit http2 settings");
  }

  ~ServerSession() { nghttp2_session_del(session); }
};

class Http2Session : public std::enable_shared_from_this<Http2Session> {
 public:
  Http2Session(std::shared_ptr<tcp::socket> socket, ServerStore* store,
               const ControlService* service,
               ControlConnectionRegistry* registry,
               ControlWorkerPool* worker_pool,
               std::shared_ptr<std::atomic_bool> done)
      : socket_(std::move(socket)),
        executor_(boost::asio::make_strand(socket_->get_executor())),
        context_(std::make_shared<ConnectionContext>(
            socket_, store, service, registry, worker_pool, executor_,
            zfleet::core::GenerateUuid())),
        async_store_(dynamic_cast<AsyncServerStore*>(store)),
        database_(dynamic_cast<ServerDatabase*>(store)),
        done_(std::move(done)) {}

  void Start() {
    auto self = shared_from_this();
    boost::asio::post(executor_, [self]() {
      self->context_->flush_output = [weak =
                                          std::weak_ptr<Http2Session>(self)] {
        if (const auto session = weak.lock()) {
          session->FlushOutput();
        }
      };
      self->context_->request_command_drain =
          [weak = std::weak_ptr<Http2Session>(self)]() {
            if (const auto session = weak.lock()) {
              session->TryStartQueuedCommands();
            }
          };
      self->context_->registry->OpenConnection(self->context_->connection_id,
                                               zfleet::core::NowUtcRfc3339());
      self->server_session_ = std::make_unique<ServerSession>(
          self->callbacks_, self->context_.get());
      if (self->database_ != nullptr) {
        self->task_queue_subscription_ =
            self->database_->SubscribeTaskQueueChanges(
                self->executor_, [weak = std::weak_ptr<Http2Session>(self)](
                                     std::uint64_t version) {
                  if (const auto session = weak.lock()) {
                    session->OnTaskQueueChanged(version);
                  }
                });
      }
      self->FlushSession();
      self->DoRead();
    });
  }

  void Stop() {
    auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->Close(); });
  }

 private:
  void DoRead() {
    if (closed_) {
      MaybeMarkDone();
      return;
    }
    read_in_flight_ = true;
    auto self = shared_from_this();
    socket_->async_read_some(boost::asio::buffer(read_buffer_),
                             boost::asio::bind_executor(
                                 executor_, [self](boost::system::error_code ec,
                                                   std::size_t bytes_read) {
                                   self->OnRead(ec, bytes_read);
                                 }));
  }

  void OnRead(boost::system::error_code ec, std::size_t bytes_read) {
    read_in_flight_ = false;
    if (closed_) {
      MaybeMarkDone();
      return;
    }
    if (ec == boost::asio::error::operation_aborted) {
      MaybeMarkDone();
      return;
    }
    if (ec == boost::asio::error::bad_descriptor) {
      Close();
      return;
    }
    if (ec == boost::asio::error::eof) {
      Close();
      return;
    }
    if (ec) {
      ZFLOG_ERROR(zfleet::core::log::Component("server").With(
                      {{"connection_id", context_->connection_id}}),
                  "http2 control connection failed: {}", ec.message());
      Close();
      return;
    }

    try {
      const auto rv = nghttp2_session_mem_recv(context_->session,
                                               read_buffer_.data(), bytes_read);
      CheckNghttp2(static_cast<int>(rv), "receive http2 bytes");
      FlushSession();
    } catch (const std::exception& ex) {
      ZFLOG_ERROR(zfleet::core::log::Component("server").With(
                      {{"connection_id", context_->connection_id}}),
                  "http2 control connection failed: {}", ex.what());
      Close();
      return;
    }
    DoRead();
  }

  void OnTaskQueueChanged(std::uint64_t version) {
    if (closed_) {
      return;
    }
    (void)version;
    for (auto& [_, stream] : context_->streams) {
      if (stream.command_stream) {
        stream.command_refresh_pending = true;
      }
    }
    TryStartQueuedCommands();
  }

  void FlushSession() {
    if (closed_ || context_->session == nullptr) {
      return;
    }
    CheckNghttp2(nghttp2_session_send(context_->session), "send http2 bytes");
    FlushOutput();
  }

  void FlushOutput() {
    if (closed_ || context_->write_in_progress ||
        context_->pending_writes.empty()) {
      return;
    }
    context_->write_in_progress = true;
    auto self = shared_from_this();
    boost::asio::async_write(
        *socket_, boost::asio::buffer(context_->pending_writes.front()),
        boost::asio::bind_executor(
            executor_, [self](boost::system::error_code ec,
                              std::size_t /*bytes*/) { self->OnWrite(ec); }));
  }

  void OnWrite(boost::system::error_code ec) {
    context_->write_in_progress = false;
    if (closed_) {
      if (!context_->pending_writes.empty()) {
        context_->pending_writes.pop_front();
      }
      MaybeMarkDone();
      return;
    }
    if (ec == boost::asio::error::operation_aborted) {
      MaybeMarkDone();
      return;
    }
    if (ec == boost::asio::error::bad_descriptor) {
      Close();
      return;
    }
    if (ec) {
      Close();
      return;
    }
    if (!context_->pending_writes.empty()) {
      context_->pending_writes.pop_front();
    }
    FinalizeSentCommandData(context_.get());
    FlushOutput();
    TryStartQueuedCommands();
  }

  void MaybeMarkDone() {
    if (!closed_ || read_in_flight_ || context_->write_in_progress ||
        offline_mark_in_flight_) {
      return;
    }
    if (done_ != nullptr) {
      done_->store(true);
    }
  }

  void TryStartQueuedCommands() {
    if (closed_ || context_->session == nullptr || async_store_ == nullptr) {
      return;
    }

    const auto connection =
        context_->registry->FindByConnection(context_->connection_id);
    if (!connection.has_value() || !connection->agent_id.has_value() ||
        connection->disconnected_at.has_value()) {
      return;
    }

    for (auto& [stream_id, stream] : context_->streams) {
      if (!stream.command_stream || !stream.command_refresh_pending ||
          stream.command_claim_in_flight || stream.command_data_in_flight ||
          !stream.response_body.empty()) {
        continue;
      }

      stream.command_refresh_pending = false;
      stream.command_claim_in_flight = true;
      async_store_->AsyncClaimNextTaskForAgent(
          *connection->agent_id, zfleet::core::NowUtcRfc3339(), executor_,
          [weak = std::weak_ptr<Http2Session>(shared_from_this()), stream_id](
              std::exception_ptr error,
              std::optional<zfleet::protocol::Task> task) {
            if (const auto session = weak.lock()) {
              session->OnCommandClaimComplete(stream_id, std::move(error),
                                              std::move(task));
            }
          });
    }
  }

  void OnCommandClaimComplete(std::int32_t stream_id, std::exception_ptr error,
                              std::optional<zfleet::protocol::Task> task) {
    if (closed_ || context_->session == nullptr) {
      return;
    }

    auto* stream = FindStream(context_.get(), stream_id);
    if (stream == nullptr) {
      return;
    }
    stream->command_claim_in_flight = false;

    if (error != nullptr) {
      ZFLOG_ERROR(zfleet::core::log::Component("server").With(
                      {{"connection_id", context_->connection_id},
                       {"stream_id", std::to_string(stream_id)}}),
                  "http2 command claim failed");
      stream->has_error = true;
      Close();
      return;
    }

    if (!task.has_value()) {
      if (stream->command_refresh_pending) {
        TryStartQueuedCommands();
      }
      return;
    }

    stream->response_body = EncodeCommandFrame(*task, stream->correlation_id);
    stream->response_offset = 0;
    SubmitCommandData(context_->session, stream_id, stream);
    FlushSession();
  }

  void Close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    boost::system::error_code ignored;
    socket_->cancel(ignored);
    socket_->shutdown(tcp::socket::shutdown_both, ignored);
    socket_->close(ignored);
    context_->flush_output = nullptr;
    context_->request_command_drain = nullptr;
    context_->session = nullptr;
    if (database_ != nullptr && task_queue_subscription_.has_value()) {
      database_->UnsubscribeTaskQueueChanges(*task_queue_subscription_);
      task_queue_subscription_.reset();
    }
    const auto disconnected_at = zfleet::core::NowUtcRfc3339();
    const auto closed = context_->registry->CloseConnection(
        context_->connection_id, disconnected_at);
    if (closed.has_value() && closed->agent_id.has_value() &&
        closed->was_current_agent_connection) {
      if (async_store_ != nullptr) {
        offline_mark_in_flight_ = true;
        async_store_->AsyncMarkAgentOffline(*closed->agent_id, disconnected_at,
                                            executor_,
                                            [weak = weak_from_this()](
                                                std::exception_ptr) {
                                              if (const auto session =
                                                      weak.lock()) {
                                                session->offline_mark_in_flight_ =
                                                    false;
                                                session->MaybeMarkDone();
                                              }
                                            });
        return;
      } else {
        context_->store->MarkAgentOffline(*closed->agent_id, disconnected_at);
      }
    }
    MaybeMarkDone();
  }

  std::shared_ptr<tcp::socket> socket_;
  boost::asio::any_io_executor executor_;
  std::array<std::uint8_t, kReadBufferBytes> read_buffer_{};
  SessionCallbacks callbacks_;
  std::shared_ptr<ConnectionContext> context_;
  std::unique_ptr<ServerSession> server_session_;
  AsyncServerStore* async_store_;
  ServerDatabase* database_;
  std::optional<ServerDatabase::TaskQueueSubscription> task_queue_subscription_;
  std::shared_ptr<std::atomic_bool> done_;
  bool read_in_flight_ = false;
  bool offline_mark_in_flight_ = false;
  bool closed_ = false;
};

}  // namespace

ControlWorkerPool::ControlWorkerPool(std::size_t thread_count) {
  if (thread_count == 0) {
    thread_count = 1;
  }
  threads_.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    threads_.emplace_back(&ControlWorkerPool::RunWorker, this);
  }
}

ControlWorkerPool::~ControlWorkerPool() { Stop(); }

std::future<std::vector<ControlEventResult>> ControlWorkerPool::Submit(
    std::function<std::vector<ControlEventResult>()> task) {
  auto promise =
      std::make_shared<std::promise<std::vector<ControlEventResult>>>();
  auto result = promise->get_future();
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      throw std::runtime_error("http2 control worker pool is stopped");
    }
    tasks_.push_back([promise, task = std::move(task)]() mutable {
      try {
        promise->set_value(task());
      } catch (...) {
        promise->set_exception(std::current_exception());
      }
    });
  }
  cv_.notify_one();
  return result;
}

bool ControlWorkerPool::Submit(
    std::function<std::vector<ControlEventResult>()> task,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr, std::vector<ControlEventResult>)>
        completion) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      boost::asio::post(
          completion_executor, [completion = std::move(completion)]() mutable {
            completion(std::make_exception_ptr(std::runtime_error(
                           "http2 control worker pool is stopped")),
                       {});
          });
      return false;
    }
    tasks_.push_back([task = std::move(task), completion_executor,
                      completion = std::move(completion)]() mutable {
      try {
        auto result = task();
        boost::asio::post(completion_executor,
                          [completion = std::move(completion),
                           result = std::move(result)]() mutable {
                            completion(nullptr, std::move(result));
                          });
      } catch (...) {
        const auto exception = std::current_exception();
        boost::asio::post(completion_executor,
                          [completion = std::move(completion),
                           exception]() mutable { completion(exception, {}); });
      }
    });
  }
  cv_.notify_one();
  return true;
}

void ControlWorkerPool::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

void ControlWorkerPool::RunWorker() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&]() { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

ControlServer::ControlServer(std::string listen_address, ServerStore* store,
                             const ControlService* service,
                             ControlConnectionRegistry* registry,
                             ControlServerOptions options)
    : endpoint_(ParseListenAddress(listen_address)),
      io_context_(1),
      acceptor_(io_context_),
      accept_strand_(io_context_.get_executor()),
      store_(store),
      service_(service),
      registry_(registry),
      options_(options),
      worker_pool_(options_.worker_threads) {
  std::fprintf(stderr,
               "[diag] ControlServer ctor this=%p sessions_addr=%p "
               "sessions_data=%p size=%zu capacity=%zu stopping_addr=%p\n",
               static_cast<void*>(this), static_cast<void*>(&sessions_),
               static_cast<void*>(sessions_.data()), sessions_.size(),
               sessions_.capacity(), static_cast<void*>(&stopping_));
}

ControlServer::~ControlServer() { Stop(); }

ControlServer::ActiveSession::ActiveSession(
    std::shared_ptr<void> session_value,
    std::function<void()> stop_value,
    std::shared_ptr<std::atomic_bool> done_value)
    : session(std::move(session_value)),
      stop(std::move(stop_value)),
      done(std::move(done_value)) {
  std::fprintf(stderr,
               "[diag] ActiveSession ctor this=%p session=%p done=%p\n",
               static_cast<void*>(this), session.get(), done.get());
}

void ControlServer::Run() {
  std::fprintf(stderr,
               "[diag] Run begin this=%p sessions_addr=%p sessions_data=%p "
               "size=%zu capacity=%zu stopping_addr=%p\n",
               static_cast<void*>(this), static_cast<void*>(&sessions_),
               static_cast<void*>(sessions_.data()), sessions_.size(),
               sessions_.capacity(), static_cast<void*>(&stopping_));
  if (store_ == nullptr) {
    throw std::invalid_argument("server store must not be null");
  }
  if (service_ == nullptr) {
    throw std::invalid_argument("http2 control service must not be null");
  }
  if (registry_ == nullptr) {
    throw std::invalid_argument("http2 connection registry must not be null");
  }

  acceptor_.open(endpoint_.protocol());
  acceptor_.set_option(tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint_);
  acceptor_.listen();
  endpoint_ = acceptor_.local_endpoint();
  stopping_.store(false);

  ZFLOG_INFO(zfleet::core::log::Component("server").With(
                 {{"control_listen_port", std::to_string(endpoint_.port())}}),
             "http2 control server started");

  if (options_.io_threads == 0) {
    options_.io_threads = 1;
  }
  StartAccept();
  {
    std::lock_guard lock(io_threads_mutex_);
    io_threads_.reserve(options_.io_threads > 0 ? options_.io_threads - 1 : 0);
    for (std::size_t index = 1; index < options_.io_threads; ++index) {
      io_threads_.emplace_back([this]() { io_context_.run(); });
    }
  }
  io_context_.run();
}

void ControlServer::Stop() {
  stopping_.store(true);

  auto close_done = std::make_shared<std::promise<void>>();
  auto close_future = close_done->get_future();
  boost::asio::post(accept_strand_, [this, close_done]() {
    boost::system::error_code ec;
    acceptor_.cancel(ec);
    acceptor_.close(ec);
    close_done->set_value();
  });
  close_future.wait_for(std::chrono::seconds(2));

  std::vector<ActiveSession> sessions;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions = std::move(sessions_);
    sessions_.clear();
  }

  for (auto& session : sessions) {
    if (session.stop) {
      session.stop();
    }
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto all_done = std::all_of(
        sessions.begin(), sessions.end(), [](const ActiveSession& session) {
          return session.done == nullptr || session.done->load();
        });
    if (all_done) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  io_context_.stop();
  worker_pool_.Stop();
  std::vector<std::thread> io_threads;
  {
    std::lock_guard lock(io_threads_mutex_);
    io_threads = std::move(io_threads_);
    io_threads_.clear();
  }
  for (auto& thread : io_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

std::uint16_t ControlServer::port() const noexcept { return endpoint_.port(); }

void ControlServer::StartAccept() {
  boost::asio::dispatch(accept_strand_,
                        [this]() { StartAcceptOnStrand(); });
}

void ControlServer::StartAcceptOnStrand() {
  std::fprintf(stderr,
               "[diag] StartAcceptOnStrand this=%p sessions_addr=%p "
               "sessions_data=%p size=%zu capacity=%zu stopping=%d\n",
               static_cast<void*>(this), static_cast<void*>(&sessions_),
               static_cast<void*>(sessions_.data()), sessions_.size(),
               sessions_.capacity(), stopping_.load() ? 1 : 0);
  if (stopping_.load() || !acceptor_.is_open()) {
    return;
  }

  acceptor_.async_accept(boost::asio::bind_executor(
      accept_strand_, [this](boost::system::error_code ec, tcp::socket socket) {
    if (ec) {
      if (!stopping_.load() && acceptor_.is_open()) {
        StartAcceptOnStrand();
      }
      return;
    }

    ReapFinishedSessions();
    auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
    auto done = std::make_shared<std::atomic_bool>(false);
    {
      std::lock_guard lock(sessions_mutex_);
      if (sessions_.size() >= options_.max_connections) {
        socket_ptr->close(ec);
        ZFLOG_ERROR(
            zfleet::core::log::Component("server"),
            "http2 control connection rejected: too many active connections");
      } else {
        auto session = std::make_shared<Http2Session>(
            socket_ptr, store_, service_, registry_, &worker_pool_, done);
        std::fprintf(stderr,
                     "[diag] before push this=%p sessions_data=%p size=%zu "
                     "capacity=%zu session=%p done=%p\n",
                     static_cast<void*>(this),
                     static_cast<void*>(sessions_.data()), sessions_.size(),
                     sessions_.capacity(), session.get(), done.get());
        sessions_.push_back(ActiveSession(
            session, [session]() { session->Stop(); }, done));
        std::fprintf(stderr,
                     "[diag] after push this=%p sessions_data=%p size=%zu "
                     "capacity=%zu back_session=%p back_done=%p\n",
                     static_cast<void*>(this),
                     static_cast<void*>(sessions_.data()), sessions_.size(),
                     sessions_.capacity(), sessions_.back().session.get(),
                     sessions_.back().done.get());
        session->Start();
      }
    }
    if (!stopping_.load() && acceptor_.is_open()) {
      StartAcceptOnStrand();
    }
  }));
}

void ControlServer::ReapFinishedSessions() {
  std::lock_guard lock(sessions_mutex_);
  auto session = sessions_.begin();
  while (session != sessions_.end()) {
    if (session->done != nullptr && session->done->load()) {
      session = sessions_.erase(session);
      continue;
    }
    ++session;
  }
}

}  // namespace zfleet::server
