#pragma once

#include "http_handler.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <string>

namespace zfleet::server {

class HttpServer {
 public:
  HttpServer(std::string listen_address, const HttpHandler* handler);

  void Run();
  void Stop();
  std::uint16_t port() const noexcept;

 private:
  void StartAccept();

  boost::asio::ip::tcp::endpoint endpoint_;
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  const HttpHandler* handler_;
};

} // namespace zfleet::server
