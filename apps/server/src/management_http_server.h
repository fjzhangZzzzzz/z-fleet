#pragma once

#include "database.h"
#include "static_file_service.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace zfleet::server {

struct ManagementHttpServerOptions {
  std::size_t io_threads = 1;
  std::chrono::milliseconds request_timeout = std::chrono::seconds(10);
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_body_bytes = 128 * 1024 * 1024;
};

class ManagementHttpServer {
 public:
  ManagementHttpServer(std::string listen_address,
                       ServerDatabase* database,
                       std::filesystem::path package_repository,
                       std::filesystem::path web_static_dir,
                       ManagementHttpServerOptions options = {});
  ~ManagementHttpServer();

  ManagementHttpServer(const ManagementHttpServer&) = delete;
  ManagementHttpServer& operator=(const ManagementHttpServer&) = delete;

  void Run();
  void Start();
  void Stop();
  std::uint16_t port() const noexcept;

 private:
  void StartAccept();

  boost::asio::ip::tcp::endpoint endpoint_;
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  ServerDatabase* database_;
  std::filesystem::path package_repository_;
  StaticFileService static_files_;
  ManagementHttpServerOptions options_;
  std::atomic_bool stopping_ = false;
  std::vector<std::thread> io_threads_;
};

}  // namespace zfleet::server
