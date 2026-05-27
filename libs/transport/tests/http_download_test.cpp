#include "zfleet/transport/http_download.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

class SingleResponseServer {
 public:
  explicit SingleResponseServer(std::string body)
      : acceptor_(io_context_,
                  {boost::asio::ip::make_address("127.0.0.1"), 0}),
        body_(std::move(body)),
        thread_([this]() { Serve(); }) {}

  ~SingleResponseServer() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::string url() const {
    return "http://127.0.0.1:" +
           std::to_string(acceptor_.local_endpoint().port()) + "/package.zip";
  }

 private:
  void Serve() {
    boost::asio::ip::tcp::socket socket(io_context_);
    acceptor_.accept(socket);
    std::array<char, 4096> request{};
    boost::system::error_code ignored;
    socket.read_some(boost::asio::buffer(request), ignored);
    const auto response =
        "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: " +
        std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
    boost::asio::write(socket, boost::asio::buffer(response));
  }

  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::string body_;
  std::thread thread_;
};

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("download http stream delivers chunks without buffering whole body") {
  const std::string body(128 * 1024, 'x');
  SingleResponseServer server(body);
  std::size_t total = 0;
  int status = zfleet::transport::DownloadHttpStream(
      {.url = server.url(), .user_agent = "zfleet_test"},
      [&total](std::span<const std::uint8_t> chunk) { total += chunk.size(); });
  REQUIRE(status == 200);
  REQUIRE(total == body.size());
}

TEST_CASE("download http to file writes response body") {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "zfleet-http-download-test.bin";
  const std::string body = "streamed-body";
  SingleResponseServer server(body);

  const auto status = zfleet::transport::DownloadHttpToFile(
      {.url = server.url(), .user_agent = "zfleet_test"}, path);

  REQUIRE(status == 200);
  REQUIRE(ReadBytes(path) == body);
  std::filesystem::remove(path);
}
