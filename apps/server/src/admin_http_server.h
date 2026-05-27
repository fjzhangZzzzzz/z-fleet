#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "static_file_service.h"

namespace zfleet::server {

struct AdminHttpServerOptions {
  std::size_t io_threads = 1;
  std::chrono::milliseconds request_timeout = std::chrono::seconds(10);
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_body_bytes = 128 * 1024 * 1024;
  bool allow_high_risk_write = false;
  std::string package_download_base_url;
  std::string control_url;
  std::filesystem::path web_static_root;
};

class AdminHttpServer {
 public:
  AdminHttpServer(std::string listen_address, ServerDatabase* database,
                  std::filesystem::path package_repository,
                  std::filesystem::path web_static_dir,
                  AdminHttpServerOptions options = {});
  ~AdminHttpServer();

  AdminHttpServer(const AdminHttpServer&) = delete;
  AdminHttpServer& operator=(const AdminHttpServer&) = delete;

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
  AdminHttpServerOptions options_;
  std::atomic_bool stopping_ = false;
  std::vector<std::thread> io_threads_;
};

}  // namespace zfleet::server
