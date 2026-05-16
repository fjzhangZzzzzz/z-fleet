#include "http_server.h"

#include "zfleet/core/log.h"

#include <boost/asio/ip/address.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <stdexcept>

namespace zfleet::server {
namespace {

namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

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

void HandleSession(tcp::socket socket, const HttpHandler& handler) {
  boost::beast::flat_buffer buffer;
  http::request<http::string_body> request;
  http::read(socket, buffer, request);
  auto response = handler.Handle(request);
  http::write(socket, response);
  boost::system::error_code ignored_error;
  socket.shutdown(tcp::socket::shutdown_send, ignored_error);
}

} // namespace

HttpServer::HttpServer(std::string listen_address, const HttpHandler* handler)
    : endpoint_(ParseListenAddress(listen_address)),
      io_context_(1),
      acceptor_(io_context_),
      handler_(handler) {}

void HttpServer::Run() {
  if (handler_ == nullptr) {
    throw std::invalid_argument("http handler must not be null");
  }

  acceptor_.open(endpoint_.protocol());
  acceptor_.set_option(tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint_);
  acceptor_.listen();
  endpoint_ = acceptor_.local_endpoint();

  ZFLOG_INFO(zfleet::core::log::Component("server").With(
                 {{"listen_port", std::to_string(endpoint_.port())}}),
             "http server started");

  StartAccept();
  io_context_.run();
}

void HttpServer::Stop() {
  boost::system::error_code ec;
  acceptor_.cancel(ec);
  acceptor_.close(ec);
  io_context_.stop();
}

std::uint16_t HttpServer::port() const noexcept {
  return endpoint_.port();
}

void HttpServer::StartAccept() {
  acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
    if (ec) {
      return;
    }

    HandleSession(std::move(socket), *handler_);
    StartAccept();
  });
}

} // namespace zfleet::server
