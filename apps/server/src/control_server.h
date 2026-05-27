#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
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

#include "control_connection_registry.h"
#include "control_dispatcher.h"
#include "control_service.h"
#include "database.h"

namespace zfleet::server {

struct ControlServerOptions {
  std::size_t io_threads = 2;
  std::size_t max_connections = 128;
  std::size_t worker_threads = 4;
};

class ControlWorkerPool {
 public:
  explicit ControlWorkerPool(std::size_t thread_count);
  ~ControlWorkerPool();

  ControlWorkerPool(const ControlWorkerPool&) = delete;
  ControlWorkerPool& operator=(const ControlWorkerPool&) = delete;

  std::future<std::vector<ControlEventResult>> Submit(
      std::function<std::vector<ControlEventResult>()> task);
  bool Submit(
      std::function<std::vector<ControlEventResult>()> task,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr, std::vector<ControlEventResult>)>
          completion);
  void Stop();

 private:
  void RunWorker();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

class ControlServer {
 public:
  ControlServer(std::string listen_address, ServerStore* store,
                const ControlService* service,
                ControlConnectionRegistry* registry,
                ControlServerOptions options = {});
  ~ControlServer();

  void Run();
  void Stop();
  std::uint16_t port() const noexcept;

 private:
  struct ActiveSession {
    std::shared_ptr<void> session;
    std::function<void()> stop;
    std::shared_ptr<std::atomic_bool> done;
  };

  void StartAccept();
  void ReapFinishedSessions();

  boost::asio::ip::tcp::endpoint endpoint_;
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  ServerStore* store_;
  const ControlService* service_;
  ControlConnectionRegistry* registry_;
  ControlServerOptions options_;
  ControlWorkerPool worker_pool_;
  std::mutex io_threads_mutex_;
  std::vector<std::thread> io_threads_;
  std::mutex sessions_mutex_;
  std::vector<ActiveSession> sessions_;
};

}  // namespace zfleet::server
