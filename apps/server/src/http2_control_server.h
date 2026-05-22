#pragma once

#include "http2_connection_registry.h"
#include "http2_control_service.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <string>

namespace zfleet::server {

class Http2ControlServer {
 public:
  Http2ControlServer(std::string listen_address,
                     const Http2ControlService* service,
                     Http2ConnectionRegistry* registry);

  void Run();
  void Stop();
  std::uint16_t port() const noexcept;

 private:
  void StartAccept();

  boost::asio::ip::tcp::endpoint endpoint_;
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  const Http2ControlService* service_;
  Http2ConnectionRegistry* registry_;
};

} // namespace zfleet::server
