#pragma once

#include "database.h"
#include "http2_connection_registry.h"
#include "http2_control_dispatcher.h"
#include "http2_control_service.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zfleet::server {

struct Http2ControlServerOptions {
  std::size_t max_connections = 128;
  std::size_t worker_threads = 4;
};

class Http2ControlWorkerPool {
 public:
  explicit Http2ControlWorkerPool(std::size_t thread_count);
  ~Http2ControlWorkerPool();

  Http2ControlWorkerPool(const Http2ControlWorkerPool&) = delete;
  Http2ControlWorkerPool& operator=(const Http2ControlWorkerPool&) = delete;

  std::future<std::vector<ControlEventResult>> Submit(
      std::function<std::vector<ControlEventResult>()> task);
  void Stop();

 private:
  void RunWorker();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

class Http2ControlServer {
 public:
  Http2ControlServer(std::string listen_address,
                     ServerStore* store,
                     const Http2ControlService* service,
                     Http2ConnectionRegistry* registry,
                     Http2ControlServerOptions options = {});
  ~Http2ControlServer();

  void Run();
  void Stop();
  std::uint16_t port() const noexcept;

 private:
  struct ConnectionThread {
    std::thread thread;
    std::shared_ptr<boost::asio::ip::tcp::socket> socket;
    std::shared_ptr<std::atomic_bool> done;
  };

  void StartAccept();
  void ReapFinishedConnections();

  boost::asio::ip::tcp::endpoint endpoint_;
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  ServerStore* store_;
  const Http2ControlService* service_;
  Http2ConnectionRegistry* registry_;
  Http2ControlServerOptions options_;
  Http2ControlWorkerPool worker_pool_;
  std::mutex connection_threads_mutex_;
  std::vector<ConnectionThread> connection_threads_;
};

} // namespace zfleet::server
