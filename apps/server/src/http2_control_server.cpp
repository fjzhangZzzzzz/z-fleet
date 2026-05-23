#include "http2_control_server.h"

#include "http2_control_dispatcher.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/message.h"
#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/write.hpp>
#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <stdexcept>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace zfleet::server {
namespace {

using tcp = boost::asio::ip::tcp;
namespace proto = zfleet::protocol::v1;

constexpr std::size_t kReadBufferBytes = 16 * 1024;
constexpr auto kTaskQueueShutdownPoll = std::chrono::seconds(1);

#define ZFLEET_NGHTTP2_NV(NAME, VALUE)                                      \
  nghttp2_nv {                                                              \
    reinterpret_cast<std::uint8_t*>(const_cast<char*>(NAME)),               \
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(VALUE)),          \
        sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE           \
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
  std::unique_ptr<Http2ControlDispatcher> dispatcher;
  std::vector<std::uint8_t> response_body;
  std::size_t response_offset = 0;
  bool has_error = false;
  bool command_stream = false;
  bool command_headers_submitted = false;
  bool command_data_in_flight = false;
};

struct ConnectionContext {
  std::shared_ptr<tcp::socket> socket;
  ServerStore* store;
  const Http2ControlService* service;
  Http2ConnectionRegistry* registry;
  Http2ControlWorkerPool* worker_pool;
  std::string connection_id;
  std::map<std::int32_t, StreamState> streams;
  std::atomic_uint64_t task_queue_version = 0;

  ConnectionContext(std::shared_ptr<tcp::socket> accepted_socket,
                    ServerStore* store_arg,
                    const Http2ControlService* service_arg,
                    Http2ConnectionRegistry* registry_arg,
                    Http2ControlWorkerPool* worker_pool_arg,
                    std::string connection_id_arg)
      : socket(std::move(accepted_socket)),
        store(store_arg),
        service(service_arg),
        registry(registry_arg),
        worker_pool(worker_pool_arg),
        connection_id(std::move(connection_id_arg)) {}
};

void CheckNghttp2(int rv, std::string_view operation) {
  if (rv < 0) {
    throw std::runtime_error(std::string(operation) + " failed: " +
                             nghttp2_strerror(rv));
  }
}

StreamState* FindStream(ConnectionContext* context, std::int32_t stream_id) {
  const auto stream = context->streams.find(stream_id);
  if (stream == context->streams.end()) {
    return nullptr;
  }
  return &stream->second;
}

proto::TaskType ToProtoTaskType(zfleet::protocol::TaskType type) {
  switch (type) {
    case zfleet::protocol::TaskType::collect_basic_inventory:
      return proto::TASK_TYPE_COLLECT_BASIC_INVENTORY;
  }
  return proto::TASK_TYPE_UNSPECIFIED;
}

proto::CapabilityLevel ToProtoCapabilityLevel(
    zfleet::protocol::CapabilityLevel level) {
  switch (level) {
    case zfleet::protocol::CapabilityLevel::readonly:
      return proto::CAPABILITY_LEVEL_READONLY;
    case zfleet::protocol::CapabilityLevel::low_risk_write:
      return proto::CAPABILITY_LEVEL_LOW_RISK_WRITE;
    case zfleet::protocol::CapabilityLevel::high_risk_write:
      return proto::CAPABILITY_LEVEL_HIGH_RISK_WRITE;
    case zfleet::protocol::CapabilityLevel::shell:
      return proto::CAPABILITY_LEVEL_SHELL;
  }
  return proto::CAPABILITY_LEVEL_UNSPECIFIED;
}

std::vector<std::uint8_t> EncodeCommandFrame(
    const zfleet::protocol::Task& task,
    std::string_view correlation_id) {
  proto::ServerCommand command;
  command.set_protocol_version(std::string(zfleet::protocol::protocol_version()));
  command.set_message_id(zfleet::core::GenerateUuid());
  command.set_correlation_id(std::string(correlation_id));
  command.set_agent_id(task.agent_id);
  command.set_occurred_at(zfleet::core::NowUtcRfc3339());
  auto* assigned = command.mutable_task_assigned();
  assigned->set_task_id(task.task_id);
  assigned->set_task_type(ToProtoTaskType(task.task_type));
  assigned->set_capability_level(ToProtoCapabilityLevel(task.capability_level));
  assigned->set_created_at(task.created_at);
  assigned->set_expires_at(task.expires_at);
  if (std::holds_alternative<zfleet::protocol::CollectBasicInventoryInput>(
          task.input)) {
    assigned->mutable_collect_basic_inventory();
  }

  std::string bytes;
  if (!command.SerializeToString(&bytes)) {
    throw std::runtime_error("serialize server command failed");
  }
  return zfleet::transport::EncodeFrame(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

void SubmitSimpleResponse(nghttp2_session* session,
                          std::int32_t stream_id,
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
                             std::int32_t /*stream_id*/,
                             std::uint8_t* buffer,
                             std::size_t length,
                             std::uint32_t* data_flags,
                             nghttp2_data_source* source,
                             void* /*user_data*/) {
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

void SubmitCommandHeaders(nghttp2_session* session,
                          std::int32_t stream_id,
                          StreamState* stream) {
  if (stream->command_headers_submitted) {
    return;
  }
  std::array<nghttp2_nv, 2> headers{
      ZFLEET_NGHTTP2_NV(":status", "200"),
      ZFLEET_NGHTTP2_NV("content-type", "application/x-protobuf"),
  };
  CheckNghttp2(nghttp2_submit_headers(session, NGHTTP2_FLAG_NONE, stream_id,
                                      nullptr, headers.data(), headers.size(),
                                      nullptr),
               "submit command stream headers");
  stream->command_headers_submitted = true;
}

void SubmitCommandData(nghttp2_session* session,
                       std::int32_t stream_id,
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
    CheckNghttp2(nghttp2_submit_data(session, NGHTTP2_FLAG_NONE, stream_id,
                                     &provider),
                 "submit command data");
  }
  stream->command_data_in_flight = true;
}

ssize_t SendCallback(nghttp2_session* /*session*/,
                     const std::uint8_t* data,
                     std::size_t length,
                     int /*flags*/,
                     void* user_data) {
  auto* context = static_cast<ConnectionContext*>(user_data);
  boost::system::error_code ec;
  boost::asio::write(*context->socket, boost::asio::buffer(data, length),
                     boost::asio::transfer_all(), ec);
  if (ec) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  return static_cast<ssize_t>(length);
}

int OnBeginHeadersCallback(nghttp2_session* /*session*/,
                           const nghttp2_frame* frame,
                           void* user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  auto* context = static_cast<ConnectionContext*>(user_data);
  context->streams.insert_or_assign(frame->hd.stream_id, StreamState{});
  return 0;
}

int OnHeaderCallback(nghttp2_session* /*session*/,
                     const nghttp2_frame* frame,
                     const std::uint8_t* name,
                     std::size_t name_length,
                     const std::uint8_t* value,
                     std::size_t value_length,
                     std::uint8_t /*flags*/,
                     void* user_data) {
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
                             ConnectionContext* context,
                             StreamState* stream) {
  if (stream == nullptr || stream->method != "GET" ||
      stream->path != zfleet::transport::kControlCommandsPath) {
    SubmitSimpleResponse(session, frame->hd.stream_id, "400");
    return;
  }

  const auto connection = context->registry->FindByConnection(
      context->connection_id);
  if (!connection.has_value() || !connection->agent_id.has_value() ||
      connection->disconnected_at.has_value()) {
    SubmitSimpleResponse(session, frame->hd.stream_id, "404");
    return;
  }

  stream->command_stream = true;
  SubmitCommandHeaders(session, frame->hd.stream_id, stream);
}

bool TrySubmitQueuedCommand(nghttp2_session* session,
                            std::int32_t stream_id,
                            ConnectionContext* context,
                            StreamState* stream) {
  if (stream == nullptr || !stream->command_stream ||
      stream->command_data_in_flight || !stream->response_body.empty()) {
    return false;
  }

  const auto connection = context->registry->FindByConnection(
      context->connection_id);
  if (!connection.has_value() || !connection->agent_id.has_value() ||
      connection->disconnected_at.has_value()) {
    return false;
  }

  const auto assigned_at = zfleet::core::NowUtcRfc3339();
  const auto task = context->store->ClaimNextTaskForAgent(
      *connection->agent_id, assigned_at);
  if (!task.has_value()) {
    return false;
  }

  stream->response_body = EncodeCommandFrame(*task, stream->correlation_id);
  stream->response_offset = 0;
  SubmitCommandData(session, stream_id, stream);
  return true;
}

bool TrySubmitQueuedCommands(nghttp2_session* session,
                             ConnectionContext* context) {
  bool submitted = false;
  for (auto& [stream_id, stream] : context->streams) {
    submitted =
        TrySubmitQueuedCommand(session, stream_id, context, &stream) ||
        submitted;
  }
  return submitted;
}

void FinalizeSentCommandData(ConnectionContext* context) {
  for (auto& [_, stream] : context->streams) {
    if (stream.command_stream && stream.command_data_in_flight &&
        !stream.response_body.empty() &&
        stream.response_offset == stream.response_body.size()) {
      stream.response_body.clear();
      stream.response_offset = 0;
      stream.command_data_in_flight = false;
    }
  }
}

void FlushQueuedCommands(nghttp2_session* session,
                         ConnectionContext* context) {
  while (TrySubmitQueuedCommands(session, context)) {
    CheckNghttp2(nghttp2_session_send(session), "send http2 bytes");
    FinalizeSentCommandData(context);
  }
}

int OnDataChunkRecvCallback(nghttp2_session* /*session*/,
                            std::uint8_t /*flags*/,
                            std::int32_t stream_id,
                            const std::uint8_t* data,
                            std::size_t length,
                            void* user_data) {
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
    stream->dispatcher = std::make_unique<Http2ControlDispatcher>(
        context->service, context->registry, context->connection_id);
  }
  std::vector<std::uint8_t> bytes(data, data + length);
  const auto results = context->worker_pool
                           ->Submit([dispatcher = stream->dispatcher.get(),
                                     bytes = std::move(bytes)]() {
                             return dispatcher->PushEventBytes(bytes);
                           })
                           .get();
  for (const auto& result : results) {
    if (result.status != ControlEventStatus::kAccepted) {
      stream->has_error = true;
    }
  }
  return 0;
}

int OnFrameRecvCallback(nghttp2_session* session,
                        const nghttp2_frame* frame,
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
    SubmitSimpleResponse(session, frame->hd.stream_id,
                         stream != nullptr && !stream->has_error ? "200"
                                                                  : "400");
    return 0;
  }

  if (frame->hd.type == NGHTTP2_DATA &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    auto* context = static_cast<ConnectionContext*>(user_data);
    auto* stream = FindStream(context, frame->hd.stream_id);
    SubmitSimpleResponse(session, frame->hd.stream_id,
                         stream != nullptr && !stream->has_error ? "200"
                                                                  : "400");
  }
  return 0;
}

int OnStreamCloseCallback(nghttp2_session* /*session*/,
                          std::int32_t stream_id,
                          std::uint32_t /*error_code*/,
                          void* user_data) {
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

  ~SessionCallbacks() {
    nghttp2_session_callbacks_del(callbacks);
  }
};

struct ServerSession {
  nghttp2_session* session = nullptr;

  ServerSession(const SessionCallbacks& callbacks, ConnectionContext* context) {
    CheckNghttp2(
        nghttp2_session_server_new(&session, callbacks.callbacks, context),
        "create nghttp2 server session");
    CheckNghttp2(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0),
                 "submit http2 settings");
  }

  ~ServerSession() {
    nghttp2_session_del(session);
  }
};

void HandleConnection(std::shared_ptr<tcp::socket> socket,
                      ServerStore* store,
                      const Http2ControlService* service,
                      Http2ConnectionRegistry* registry,
                      Http2ControlWorkerPool* worker_pool) {
  ConnectionContext context(std::move(socket), store, service, registry,
                            worker_pool,
                            zfleet::core::GenerateUuid());
  context.task_queue_version.store(store->TaskQueueVersion());
  registry->OpenConnection(context.connection_id, zfleet::core::NowUtcRfc3339());

  try {
    SessionCallbacks callbacks;
    ServerSession server_session(callbacks, &context);
    std::mutex session_mutex;
    std::atomic_bool stop_task_notifier{false};
    {
      std::lock_guard lock(session_mutex);
      CheckNghttp2(nghttp2_session_send(server_session.session),
                   "send initial http2 settings");
    }

    std::thread task_notifier([&]() {
      while (!stop_task_notifier.load()) {
        const auto next_task_queue_version =
            context.store->WaitForTaskQueueChange(
                context.task_queue_version.load(), kTaskQueueShutdownPoll);
        if (next_task_queue_version == context.task_queue_version.load()) {
          continue;
        }

        std::lock_guard lock(session_mutex);
        context.task_queue_version.store(next_task_queue_version);
        FlushQueuedCommands(server_session.session, &context);
      }
    });
    struct TaskNotifierCleanup {
      std::atomic_bool& stop;
      std::thread& thread;

      ~TaskNotifierCleanup() {
        stop.store(true);
        if (thread.joinable()) {
          thread.join();
        }
      }
    } task_notifier_cleanup{stop_task_notifier, task_notifier};

    std::array<std::uint8_t, kReadBufferBytes> buffer{};
    boost::system::error_code ec;
    while (context.socket->is_open() &&
           (nghttp2_session_want_read(server_session.session) != 0 ||
            nghttp2_session_want_write(server_session.session) != 0)) {
      const auto bytes_read =
          context.socket->read_some(boost::asio::buffer(buffer), ec);
      if (ec == boost::asio::error::eof) {
        break;
      }
      if (ec) {
        throw boost::system::system_error(ec);
      }
      std::lock_guard lock(session_mutex);
      const auto rv = nghttp2_session_mem_recv(server_session.session,
                                               buffer.data(), bytes_read);
      CheckNghttp2(static_cast<int>(rv), "receive http2 bytes");
      context.task_queue_version.store(context.store->TaskQueueVersion());
      FlushQueuedCommands(server_session.session, &context);
      CheckNghttp2(nghttp2_session_send(server_session.session),
                   "send http2 bytes");
    }
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server").With(
                    {{"connection_id", context.connection_id}}),
                "http2 control connection failed: {}",
                ex.what());
  }

  const auto disconnected_at = zfleet::core::NowUtcRfc3339();
  const auto closed = registry->CloseConnection(context.connection_id,
                                                disconnected_at);
  if (closed.has_value() && closed->agent_id.has_value() &&
      closed->was_current_agent_connection) {
    store->MarkAgentOffline(*closed->agent_id, disconnected_at);
  }
}

} // namespace

Http2ControlWorkerPool::Http2ControlWorkerPool(std::size_t thread_count) {
  if (thread_count == 0) {
    thread_count = 1;
  }
  threads_.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    threads_.emplace_back(&Http2ControlWorkerPool::RunWorker, this);
  }
}

Http2ControlWorkerPool::~Http2ControlWorkerPool() {
  Stop();
}

std::future<std::vector<ControlEventResult>> Http2ControlWorkerPool::Submit(
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

void Http2ControlWorkerPool::Stop() {
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

void Http2ControlWorkerPool::RunWorker() {
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

Http2ControlServer::Http2ControlServer(
    std::string listen_address,
    ServerStore* store,
    const Http2ControlService* service,
    Http2ConnectionRegistry* registry,
    Http2ControlServerOptions options)
    : endpoint_(ParseListenAddress(listen_address)),
      io_context_(1),
      acceptor_(io_context_),
      store_(store),
      service_(service),
      registry_(registry),
      options_(options),
      worker_pool_(options_.worker_threads) {}

Http2ControlServer::~Http2ControlServer() {
  Stop();
}

void Http2ControlServer::Run() {
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

  ZFLOG_INFO(zfleet::core::log::Component("server").With(
                 {{"control_listen_port", std::to_string(endpoint_.port())}}),
             "http2 control server started");

  StartAccept();
  io_context_.run();
}

void Http2ControlServer::Stop() {
  boost::system::error_code ec;
  acceptor_.cancel(ec);
  acceptor_.close(ec);
  io_context_.stop();

  std::vector<ConnectionThread> connection_threads;
  {
    std::lock_guard lock(connection_threads_mutex_);
    for (auto& connection_thread : connection_threads_) {
      if (connection_thread.socket != nullptr) {
        connection_thread.socket->close(ec);
      }
    }
    connection_threads = std::move(connection_threads_);
    connection_threads_.clear();
  }

  for (auto& connection_thread : connection_threads) {
    if (connection_thread.thread.joinable()) {
      connection_thread.thread.join();
    }
  }

  worker_pool_.Stop();
}

std::uint16_t Http2ControlServer::port() const noexcept {
  return endpoint_.port();
}

void Http2ControlServer::StartAccept() {
  acceptor_.async_accept(
      [this](boost::system::error_code ec, tcp::socket socket) {
        if (ec) {
          if (acceptor_.is_open()) {
            StartAccept();
          }
          return;
        }

        ReapFinishedConnections();
        auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
        auto done = std::make_shared<std::atomic_bool>(false);
        {
          std::lock_guard lock(connection_threads_mutex_);
          if (connection_threads_.size() >= options_.max_connections) {
            socket_ptr->close(ec);
            ZFLOG_ERROR(zfleet::core::log::Component("server"),
                        "http2 control connection rejected: too many active connections");
          } else {
            connection_threads_.push_back(ConnectionThread{
                .thread = std::thread(
                    [socket = socket_ptr, store = store_, service = service_,
                     registry = registry_, worker_pool = &worker_pool_,
                     done]() mutable {
                      HandleConnection(std::move(socket), store, service,
                                       registry, worker_pool);
                      done->store(true);
                    }),
                .socket = socket_ptr,
                .done = done,
            });
          }
        }
        if (acceptor_.is_open()) {
          StartAccept();
        }
      });
}

void Http2ControlServer::ReapFinishedConnections() {
  std::lock_guard lock(connection_threads_mutex_);
  auto connection = connection_threads_.begin();
  while (connection != connection_threads_.end()) {
    if (connection->done != nullptr && connection->done->load()) {
      if (connection->thread.joinable()) {
        connection->thread.join();
      }
      connection = connection_threads_.erase(connection);
      continue;
    }
    ++connection;
  }
}

} // namespace zfleet::server
