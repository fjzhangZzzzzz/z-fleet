#pragma once

#include "database.h"

#include "zfleet/core/log.h"

#include <boost/beast/http.hpp>

namespace zfleet::server {

namespace http = boost::beast::http;

class HttpHandler {
 public:
  explicit HttpHandler(ServerDatabase* database);

  http::response<http::string_body> Handle(
      const http::request<http::string_body>& request) const;

 private:
  http::response<http::string_body> HandleRegister(
      const http::request<http::string_body>& request) const;
  http::response<http::string_body> HandleHeartbeat(
      const http::request<http::string_body>& request,
      std::string_view agent_id) const;
  http::response<http::string_body> HandleAssets(
      const http::request<http::string_body>& request,
      std::string_view agent_id) const;

  http::response<http::string_body> MakeStatusResponse(
      http::status status_code,
      const zfleet::protocol::StatusResponse& response) const;
  http::response<http::string_body> MakeErrorResponse(
      http::status status_code,
      zfleet::protocol::ErrorCode error_code,
      std::string_view message,
      bool retryable,
      std::string request_id,
      std::optional<std::string> agent_id) const;

  ServerDatabase* database_;
};

} // namespace zfleet::server
