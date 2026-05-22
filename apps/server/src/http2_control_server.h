#pragma once

#include "database.h"
#include "http2_connection_registry.h"
#include "http2_control_service.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zfleet::server {

class Http2ControlServer {
 public:
  Http2ControlServer(std::string listen_address,
                     ServerStore* store,
                     const Http2ControlService* service,
                     Http2ConnectionRegistry* registry);

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
  std::mutex connection_threads_mutex_;
  std::vector<ConnectionThread> connection_threads_;
};

} // namespace zfleet::server
