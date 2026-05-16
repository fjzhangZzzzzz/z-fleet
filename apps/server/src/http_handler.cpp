#include "http_handler.h"

#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/json_codec.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string_view>

namespace zfleet::server {
namespace {

using nlohmann::json;

constexpr std::string_view kRegisterRoute = "/v1/agents/register";
constexpr std::string_view kHeartbeatSuffix = "/heartbeat";
constexpr std::string_view kAssetsSuffix = "/assets";

void RecordAuditEvent(ServerDatabase* database,
                      zfleet::protocol::AuditEventType event_type,
                      std::string request_id,
                      std::optional<std::string> agent_id,
                      std::string result,
                      const json& payload) {
  database->RecordAuditEvent(zfleet::protocol::AuditEvent{
      .audit_id = zfleet::core::GenerateUuid(),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_id = std::move(agent_id),
      .request_id = std::move(request_id),
      .event_type = event_type,
      .result = std::move(result),
      .payload_json = payload.dump(),
  });
}

std::optional<std::string_view> MatchAgentRoute(std::string_view target,
                                                std::string_view suffix) {
  constexpr std::string_view kPrefix = "/v1/agents/";
  if (!target.starts_with(kPrefix) || !target.ends_with(suffix) ||
      target.size() <= kPrefix.size() + suffix.size()) {
    return std::nullopt;
  }

  const auto agent_id_size = target.size() - kPrefix.size() - suffix.size();
  return target.substr(kPrefix.size(), agent_id_size);
}

template <typename T>
T ParseBody(const std::string& body) {
  return json::parse(body).get<T>();
}

http::response<http::string_body> MakeJsonResponse(http::status status_code,
                                                   const json& body) {
  http::response<http::string_body> response{status_code, 11};
  response.set(http::field::content_type, "application/json");
  response.body() = body.dump();
  response.prepare_payload();
  return response;
}

zfleet::core::log::Context RequestLogger(std::string_view route,
                                         std::string_view request_id,
                                         std::string_view agent_id = {}) {
  auto context = zfleet::core::log::Component("server").With({{"route", route},
                                                              {"request_id",
                                                               request_id}});
  if (!agent_id.empty()) {
    context = context.With({"agent_id", agent_id});
  }
  return context;
}

} // namespace

HttpHandler::HttpHandler(ServerDatabase* database) : database_(database) {}

http::response<http::string_body> HttpHandler::Handle(
    const http::request<http::string_body>& request) const {
  if (request.method() != http::verb::post) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "unsupported method", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }

  const auto target = std::string_view(request.target().data(),
                                       request.target().size());
  if (target == kRegisterRoute) {
    return HandleRegister(request);
  }
  if (const auto agent_id = MatchAgentRoute(target, kHeartbeatSuffix);
      agent_id.has_value()) {
    return HandleHeartbeat(request, *agent_id);
  }
  if (const auto agent_id = MatchAgentRoute(target, kAssetsSuffix);
      agent_id.has_value()) {
    return HandleAssets(request, *agent_id);
  }

  return MakeErrorResponse(http::status::bad_request,
                           std::nullopt,
                           zfleet::protocol::ErrorCode::invalid_json,
                           "unsupported route", false,
                           zfleet::core::GenerateUuid(), std::nullopt);
}

http::response<http::string_body> HttpHandler::HandleRegister(
    const http::request<http::string_body>& request) const {
  try {
    const auto parsed =
        ParseBody<zfleet::protocol::RegistrationRequest>(request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_register,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }

    database_->UpsertAgent(parsed);
    RecordAuditEvent(database_, zfleet::protocol::AuditEventType::agent_register,
                     parsed.request_id, parsed.agent_id, "success",
                     json{{"http_status", 200},
                          {"status", "accepted"},
                          {"agent_version", parsed.agent_version},
                          {"hostname", parsed.hostname},
                          {"os", parsed.os},
                          {"arch", parsed.arch}});
    ZFLOG_INFO(RequestLogger("register", parsed.request_id, parsed.agent_id),
               "registration accepted");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server").With({"route", "register"}),
                "register failed: {}",
                ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::HandleHeartbeat(
    const http::request<http::string_body>& request,
    std::string_view agent_id) const {
  try {
    const auto parsed = ParseBody<zfleet::protocol::HeartbeatRequest>(
        request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (parsed.agent_id != agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::agent_id_mismatch,
                               "path agent_id does not match body agent_id",
                               false, parsed.request_id, parsed.agent_id);
    }
    if (!database_->AgentExists(parsed.agent_id)) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::agent_not_registered,
                               "agent not registered", true, parsed.request_id,
                               parsed.agent_id);
    }

    database_->RecordHeartbeat(parsed, json(parsed).dump());
    RecordAuditEvent(database_,
                     zfleet::protocol::AuditEventType::agent_heartbeat,
                     parsed.request_id,
                     parsed.agent_id,
                     "success",
                     json{{"http_status", 200},
                          {"status", "ok"},
                          {"agent_version", parsed.agent_version}});
    ZFLOG_INFO(RequestLogger("heartbeat", parsed.request_id, parsed.agent_id),
               "heartbeat stored");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "ok",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(
        zfleet::core::log::Component("server").With({"route", "heartbeat"}),
        "heartbeat failed: {}",
        ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::HandleAssets(
    const http::request<http::string_body>& request,
    std::string_view agent_id) const {
  try {
    const auto parsed =
        ParseBody<zfleet::protocol::AssetSnapshotRequest>(request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (parsed.agent_id != agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::agent_id_mismatch,
                               "path agent_id does not match body agent_id",
                               false, parsed.request_id, parsed.agent_id);
    }
    if (!database_->AgentExists(parsed.agent_id)) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::agent_not_registered,
                               "agent not registered", true, parsed.request_id,
                               parsed.agent_id);
    }

    database_->RecordAssetSnapshot(parsed, json(parsed).dump());
    RecordAuditEvent(database_,
                     zfleet::protocol::AuditEventType::agent_asset_snapshot,
                     parsed.request_id,
                     parsed.agent_id,
                     "success",
                     json{{"http_status", 200},
                          {"status", "stored"},
                          {"hostname", parsed.hostname},
                          {"os", parsed.os},
                          {"arch", parsed.arch}});
    ZFLOG_INFO(RequestLogger("assets", parsed.request_id, parsed.agent_id),
               "asset snapshot stored");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "stored",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server").With({"route", "assets"}),
                "asset snapshot failed: {}",
                ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::MakeStatusResponse(
    http::status status_code,
    const zfleet::protocol::StatusResponse& response) const {
  return MakeJsonResponse(status_code, json(response));
}

http::response<http::string_body> HttpHandler::MakeErrorResponse(
    http::status status_code,
    std::optional<zfleet::protocol::AuditEventType> event_type,
    zfleet::protocol::ErrorCode error_code,
    std::string_view message,
    bool retryable,
    std::string request_id,
    std::optional<std::string> agent_id) const {
  const auto response = zfleet::protocol::ErrorResponse{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = std::move(request_id),
      .agent_id = std::move(agent_id),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .error_code = error_code,
      .message = std::string(message),
      .retryable = retryable,
  };
  if (event_type.has_value()) {
    RecordAuditEvent(database_,
                     *event_type,
                     response.request_id,
                     response.agent_id,
                     "failure",
                     json{{"http_status", static_cast<int>(status_code)},
                          {"error_code", zfleet::protocol::ToString(error_code)},
                          {"message", response.message},
                          {"retryable", response.retryable}});
  }
  return MakeJsonResponse(status_code, json(response));
}

} // namespace zfleet::server
