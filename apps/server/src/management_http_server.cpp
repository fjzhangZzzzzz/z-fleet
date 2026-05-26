#include "management_http_server.h"

#include "install_platform_registry.h"
#include "management_json_codec.h"
#include "package_repository.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/crypto/sha256.h"
#include "zfleet/package/manifest.h"
#include "zfleet/protocol/json_codec.h"

#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace zfleet::server {
namespace {

using tcp = boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = boost::beast::http;

struct HttpRequest {
  std::string method;
  std::string target;
  std::string path;
  std::map<std::string, std::string> query;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

void RecordManagementAudit(ServerDatabase* database,
                           zfleet::protocol::AuditEventType event_type,
                           const std::string& request_id,
                           const std::string& result,
                           const std::string& payload_json) {
  database->RecordAuditEvent(zfleet::protocol::AuditEvent{
      .audit_id = zfleet::core::GenerateUuid(),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_id = std::nullopt,
      .request_id = request_id,
      .event_type = event_type,
      .result = result,
      .payload_json = payload_json,
  });
}

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

std::string UrlDecode(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      int byte = 0;
      const auto* begin = value.data() + i + 1;
      const auto* end = begin + 2;
      if (std::from_chars(begin, end, byte, 16).ec == std::errc{}) {
        decoded.push_back(static_cast<char>(byte));
        i += 2;
        continue;
      }
    }
    if (value[i] == '+') {
      decoded.push_back(' ');
    } else {
      decoded.push_back(value[i]);
    }
  }
  return decoded;
}

std::map<std::string, std::string> ParseQuery(std::string_view query) {
  std::map<std::string, std::string> values;
  while (!query.empty()) {
    const auto next = query.find('&');
    const auto item = query.substr(0, next);
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) {
      values.emplace(UrlDecode(item), "");
    } else {
      values.emplace(UrlDecode(item.substr(0, equals)),
                     UrlDecode(item.substr(equals + 1)));
    }
    if (next == std::string_view::npos) {
      break;
    }
    query.remove_prefix(next + 1);
  }
  return values;
}

HttpResponse JsonResponse(nlohmann::json body, int status = 200) {
  return HttpResponse{
      .status = status,
      .content_type = "application/json",
      .body = body.dump(),
  };
}

HttpResponse ErrorResponse(int status,
                           std::string code,
                           std::string message,
                           bool retryable = false) {
  return JsonResponse({{"protocol_version",
                        std::string(zfleet::protocol::protocol_version())},
                       {"occurred_at", zfleet::core::NowUtcRfc3339()},
                       {"error_code", std::move(code)},
                       {"message", std::move(message)},
                       {"retryable", retryable}},
                      status);
}

template <typename Body>
HttpRequest ToApplicationRequest(const http::request<Body>& input) {
  HttpRequest request;
  request.method = std::string(input.method_string());
  request.target = std::string(input.target());

  const auto query_start = request.target.find('?');
  if (query_start == std::string::npos) {
    request.path = UrlDecode(request.target);
  } else {
    request.path = UrlDecode(std::string_view(request.target).substr(
        0, query_start));
    request.query = ParseQuery(std::string_view(request.target).substr(
        query_start + 1));
  }
  return request;
}

http::response<http::string_body> ToBeastResponse(
    const HttpResponse& response) {
  http::response<http::string_body> output{
      static_cast<http::status>(response.status), 11};
  output.set(http::field::content_type, response.content_type);
  output.set(http::field::cache_control, "no-store");
  output.keep_alive(false);
  output.body() = response.body;
  output.prepare_payload();
  return output;
}

nlohmann::json ToJson(const AgentSummary& agent) {
  nlohmann::json body = {
      {"agent_id", agent.agent_id},
      {"first_seen_at", agent.first_seen_at},
      {"last_seen_at", agent.last_seen_at},
      {"last_online_at", agent.last_online_at},
      {"platform", agent.platform},
      {"agent_version", agent.agent_version},
      {"status", agent.status},
  };
  if (agent.last_offline_at.has_value()) {
    body["last_offline_at"] = *agent.last_offline_at;
  } else {
    body["last_offline_at"] = nullptr;
  }
  body["current_package_id"] = agent.current_package_id.value_or("");
  body["desired_version"] = agent.desired_version.value_or("");
  body["desired_package_id"] = agent.desired_package_id.value_or("");
  body["desired_set_at"] = agent.desired_set_at.value_or("");
  body["desired_set_by"] = agent.desired_set_by.value_or("");
  body["upgrade_state"] = agent.upgrade_state.value_or("");
  body["last_upgrade_task_id"] = agent.last_upgrade_task_id.value_or("");
  body["last_upgrade_error"] = agent.last_upgrade_error.value_or("");
  body["last_upgrade_at"] = agent.last_upgrade_at.value_or("");
  return body;
}

nlohmann::json ToJson(const AssetSnapshotSummary& snapshot) {
  nlohmann::json body = {
      {"snapshot_id", snapshot.snapshot_id},
      {"agent_id", snapshot.agent_id},
      {"occurred_at", snapshot.occurred_at},
      {"hostname", snapshot.hostname},
      {"os", snapshot.os},
      {"arch", snapshot.arch},
      {"agent_version", snapshot.agent_version},
      {"applications", snapshot.applications},
      {"services", snapshot.services},
  };
  if (snapshot.os_version.has_value()) {
    body["os_version"] = *snapshot.os_version;
  } else {
    body["os_version"] = nullptr;
  }
  return body;
}

nlohmann::json ToJson(const AgentPackageRecord& package) {
  nlohmann::json body = {
      {"package_id", package.package_id},
      {"component", package.component},
      {"version", package.version},
      {"platform", package.platform},
      {"arch", package.arch},
      {"build_type", package.build_type},
      {"filename", package.filename},
      {"size_bytes", package.size_bytes},
      {"sha256", package.sha256},
      {"status", package.status},
      {"uploaded_at", package.uploaded_at},
  };
  body["validated_at"] = package.validated_at.value_or("");
  body["published_at"] = package.published_at.value_or("");
  body["retired_at"] = package.retired_at.value_or("");
  body["published_channels"] = package.published_channels;
  return body;
}

std::optional<std::array<int, 3>> ParseVersion(std::string_view value) {
  std::array<int, 3> parts{};
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto delimiter = value.find('.');
    const auto part = value.substr(0, delimiter);
    if (part.empty()) {
      return std::nullopt;
    }
    const auto* begin = part.data();
    const auto* end = begin + part.size();
    if (std::from_chars(begin, end, parts[i]).ec != std::errc{} ||
        parts[i] < 0) {
      return std::nullopt;
    }
    if (i + 1 == parts.size()) {
      if (delimiter != std::string_view::npos) {
        return std::nullopt;
      }
    } else {
      if (delimiter == std::string_view::npos) {
        return std::nullopt;
      }
      value.remove_prefix(delimiter + 1);
    }
  }
  return parts;
}

bool VersionAtLeast(std::string_view actual, std::string_view minimum) {
  const auto parsed_actual = ParseVersion(actual);
  const auto parsed_minimum = ParseVersion(minimum);
  return parsed_actual.has_value() && parsed_minimum.has_value() &&
         *parsed_actual >= *parsed_minimum;
}

nlohmann::json InstallPackageJson(const AgentPackageRecord& package) {
  return {
      {"package_id", package.package_id},
      {"version", package.version},
      {"sha256", package.sha256},
      {"download_url", "/api/v1/packages/" + package.component + "/" +
                           package.package_id + "/download"},
  };
}

std::string QueryValue(const HttpRequest& request,
                       std::string_view name,
                       std::string fallback) {
  if (const auto it = request.query.find(std::string(name));
      it != request.query.end() && !it->second.empty()) {
    return it->second;
  }
  return fallback;
}

std::string PathTail(std::string_view path, std::string_view prefix) {
  if (path.rfind(prefix, 0) != 0) {
    return {};
  }
  return UrlDecode(path.substr(prefix.size()));
}

std::filesystem::path SafePackageStoragePath(
    const std::filesystem::path& repository,
    std::string_view package_id) {
  if (package_id.empty() || package_id.find('/') != std::string_view::npos ||
      package_id.find('\\') != std::string_view::npos ||
      package_id.find("..") != std::string_view::npos) {
    throw std::invalid_argument("package id is invalid");
  }
  return repository / (std::string(package_id) + ".zip");
}

HttpResponse CommitAgentPackageUpload(
    const HttpRequest& request,
    ServerDatabase* database,
    const std::filesystem::path& package_repository,
    const std::filesystem::path& staging_path,
    const std::string& package_id,
    std::uintmax_t uploaded_bytes) {
  if (uploaded_bytes == 0) {
    std::error_code ignored;
    std::filesystem::remove(staging_path, ignored);
    return ErrorResponse(400, "package_body_empty",
                         "package upload body is empty");
  }

  const auto filename = QueryValue(request, "filename", "");
  const auto now = zfleet::core::NowUtcRfc3339();
  try {
    const auto metadata = ValidateAgentPackageUpload(staging_path, filename);
    std::filesystem::create_directories(package_repository);
    const auto storage_path =
        SafePackageStoragePath(package_repository, package_id);
    std::filesystem::rename(staging_path, storage_path);
    AgentPackageRecord record{
        .package_id = package_id,
        .component = metadata.component,
        .version = metadata.version,
        .platform = metadata.platform,
        .arch = metadata.arch,
        .build_type = metadata.build_type,
        .filename = filename,
        .storage_path = storage_path,
        .size_bytes = metadata.size_bytes,
        .sha256 = metadata.sha256,
        .manifest_json = metadata.manifest_json,
        .status = "validated",
        .uploaded_at = now,
        .validated_at = now,
    };
    database->UpsertAgentPackage(record);
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::package_validated,
        package_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"package_id", package_id},
             {"version", metadata.version},
             {"platform", metadata.platform},
             {"arch", metadata.arch},
             {"build_type", metadata.build_type},
             {"component", metadata.component}}));
    return JsonResponse(ToJson(record), 201);
  } catch (const std::exception& ex) {
    std::error_code ignored;
    std::filesystem::remove(staging_path, ignored);
    return ErrorResponse(400, "package_validation_failed", ex.what());
  }
}

struct ApiContext {
  const HttpRequest& request;
  ServerDatabase* database;
  const std::filesystem::path& package_repository;
  bool allow_high_risk_write;
  const std::string& package_download_base_url;
  const std::string& control_url;
  const std::filesystem::path& web_static_root;
};

std::optional<HttpResponse> HandleAgentApi(const ApiContext& ctx) {
  const auto& request = ctx.request;
  auto* database = ctx.database;
  if (request.method == "GET" && request.path == "/api/v1/agents") {
    nlohmann::json agents = nlohmann::json::array();
    for (const auto& agent : database->ListAgents()) {
      auto body = ToJson(agent);
      if (const auto latest = database->FindLatestAssetSnapshot(agent.agent_id);
          latest.has_value()) {
        body["latest_asset"] = ToJson(*latest);
      } else {
        body["latest_asset"] = nullptr;
      }
      agents.push_back(std::move(body));
    }
    return JsonResponse({{"agents", std::move(agents)}});
  }

  if (request.method == "GET" &&
      request.path.rfind("/api/v1/agents/", 0) == 0) {
    const auto rest = PathTail(request.path, "/api/v1/agents/");
    const auto delimiter = rest.find('/');
    const auto agent_id = delimiter == std::string::npos
                              ? rest
                              : rest.substr(0, delimiter);
    if (delimiter == std::string::npos) {
      const auto agent = database->FindAgent(agent_id);
      if (!agent.has_value()) {
        return ErrorResponse(404, "agent_not_found", "agent was not found");
      }
      auto body = ToJson(*agent);
      if (const auto latest = database->FindLatestAssetSnapshot(agent_id);
          latest.has_value()) {
        body["latest_asset"] = ToJson(*latest);
      } else {
        body["latest_asset"] = nullptr;
      }
      return JsonResponse(body);
    }
    const auto suffix = rest.substr(delimiter + 1);
    if (suffix == "assets/latest") {
      const auto latest = database->FindLatestAssetSnapshot(agent_id);
      if (!latest.has_value()) {
        return ErrorResponse(404, "asset_snapshot_not_found",
                             "asset snapshot was not found");
      }
      return JsonResponse(ToJson(*latest));
    }
    if (suffix == "assets") {
      nlohmann::json snapshots = nlohmann::json::array();
      for (const auto& snapshot : database->ListAssetSnapshots(agent_id, 100)) {
        snapshots.push_back(ToJson(snapshot));
      }
      return JsonResponse({{"assets", std::move(snapshots)}});
    }
  }

  return std::nullopt;
}

std::optional<HttpResponse> HandleInstallApi(const ApiContext& ctx) {
  const auto& request = ctx.request;
  auto* database = ctx.database;
  if (request.method == "GET" && request.path == "/api/v1/install/script") {
    const auto platform = QueryValue(request, "platform", "");
    if (platform.empty()) {
      return ErrorResponse(400, "missing_required_field",
                           "platform is required");
    }
    const auto* spec = FindInstallPlatformSpec(platform);
    if (spec == nullptr) {
      return ErrorResponse(400, "platform_not_allowed",
                           "platform must be linux or windows");
    }
    return HttpResponse{
        .status = 200,
        .content_type = spec->script_content_type,
        .body = RenderInstallScript(platform, ctx.web_static_root),
    };
  }

  if (request.method == "GET" && request.path == "/api/v1/install/commands") {
    const auto server_url = QueryValue(request, "server_url", "");
    const auto token = QueryValue(request, "token", "");
    const auto channel = QueryValue(request, "channel", "stable");
    const auto platform = QueryValue(request, "platform", "linux");
    const auto root = QueryValue(request, "root", "/opt/zfleet");
    if (server_url.empty() || token.empty()) {
      return ErrorResponse(400, "missing_required_field",
                           "server_url and token are required");
    }
    if (platform != "linux" && platform != "windows") {
      return ErrorResponse(400, "platform_not_allowed",
                           "platform must be linux or windows");
    }
    if (ctx.control_url.empty()) {
      return ErrorResponse(500, "control_url_not_configured",
                           "server control url is not configured");
    }
    const InstallCommandRequest command_request{
        .server_url = server_url,
        .control_url = ctx.control_url,
        .token = token,
        .channel = channel,
        .root = root,
    };
    return JsonResponse({{"platform", platform},
                         {"script_url", InstallScriptUrl(platform, server_url)},
                         {"command", BuildInstallCommand(platform, command_request)}});
  }

  if (request.method == "GET" && request.path == "/api/v1/install/options") {
    const auto channel = QueryValue(request, "channel", "stable");
    const auto platform = QueryValue(request, "platform", "linux");
    const auto arch = QueryValue(request, "arch", "x86_64");
    const auto build_type = QueryValue(request, "build_type", "release");
    if (build_type != "debug" && build_type != "release") {
      return ErrorResponse(400, "build_type_not_allowed",
                           "build_type must be debug or release");
    }
    nlohmann::json body = {
        {"server_url", ""},
        {"channel", channel},
        {"platform", platform},
        {"arch", arch},
        {"build_type", build_type},
    };
    const auto agent = database->FindDefaultPublishedAgentPackage(
        "agent", channel, platform, arch, build_type);
    if (!agent.has_value()) {
      return ErrorResponse(404, "no_agent_package",
                           "no published agent package matches install options");
    }
    const auto installer = database->FindDefaultPublishedAgentPackage(
        "installer", channel, platform, arch, build_type);
    const auto agent_manifest =
        zfleet::package::ParseManifestJson(agent->manifest_json);
    if (!installer.has_value() ||
        !VersionAtLeast(installer->version,
                        agent_manifest.min_installer_version)) {
      return ErrorResponse(409, "no_compatible_installer_package",
                           "no published installer package satisfies agent "
                           "minimum installer version");
    }
    body["agent"] = InstallPackageJson(*agent);
    body["installer"] = InstallPackageJson(*installer);
    return JsonResponse(body);
  }

  if (request.method == "POST" && request.path == "/api/v1/install/tokens") {
    const auto input = ParseTokenCreateRequestBody(request.body);
    if (input.expires_at.empty()) {
      return ErrorResponse(400, "token_expiry_required",
                           "expires_at is required");
    }
    const auto now = zfleet::core::NowUtcRfc3339();
    const auto token_id = zfleet::core::GenerateUuid();
    const auto token_value = zfleet::core::GenerateUuid();
    RegistrationTokenRecord token{
        .token_id = token_id,
        .token_hash = zfleet::crypto::Sha256BytesHex(token_value),
        .purpose = input.purpose,
        .channel = input.channel,
        .platform = input.platform,
        .arch = input.arch,
        .max_uses = input.max_uses,
        .use_count = 0,
        .status = "active",
        .created_at = now,
        .expires_at = input.expires_at,
    };
    database->CreateRegistrationToken(token);
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::registration_token_created,
        token_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"token_id", token_id}, {"purpose", token.purpose}}));
    return JsonResponse({{"token_id", token_id},
                         {"token", token_value},
                         {"expires_at", token.expires_at}},
                        201);
  }

  return std::nullopt;
}

std::optional<HttpResponse> HandleAgentMutationApi(const ApiContext& ctx) {
  const auto& request = ctx.request;
  auto* database = ctx.database;
  if (request.method == "POST" &&
      request.path.rfind("/api/v1/agents/", 0) == 0 &&
      request.path.ends_with("/rollback")) {
    if (!ctx.allow_high_risk_write) {
      return ErrorResponse(403, "capability_not_allowed",
                           "high_risk_write policy is disabled");
    }
    const auto prefix = std::string("/api/v1/agents/");
    const auto suffix = std::string("/rollback");
    const auto agent_id = request.path.substr(
        prefix.size(), request.path.size() - prefix.size() - suffix.size());
    if (!database->FindAgent(agent_id).has_value()) {
      return ErrorResponse(404, "agent_not_found", "agent was not found");
    }
    const auto input = ParseRollbackRequestBody(request.body);
    const auto now = zfleet::core::NowUtcRfc3339();
    const auto task_id = zfleet::core::GenerateUuid();
    const zfleet::protocol::Task task{
        .protocol_version = std::string(zfleet::protocol::protocol_version()),
        .task_id = task_id,
        .agent_id = agent_id,
        .task_type = zfleet::protocol::TaskType::package_update,
        .capability_level = zfleet::protocol::CapabilityLevel::high_risk_write,
        .created_at = now,
        .expires_at = input.expires_at,
        .input = zfleet::protocol::PackageUpdateInput{
            .action = "rollback",
            .component = "agent",
        },
    };
    database->ScheduleAgentRollback(agent_id, input.set_by, now, task);
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::agent_rollback_requested,
        task_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"agent_id", agent_id}, {"task_id", task_id}}));
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::task_queued,
        task_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"agent_id", agent_id},
             {"task_id", task_id},
             {"task_type", std::string("package_update")},
             {"action", std::string("rollback")}}));
    return JsonResponse({{"task_id", task_id},
                         {"agent_id", agent_id},
                         {"upgrade_state", "queued"},
                         {"action", "rollback"}},
                        201);
  }

  if (request.method == "POST" &&
      request.path.rfind("/api/v1/agents/", 0) == 0 &&
      request.path.ends_with("/upgrade")) {
    if (!ctx.allow_high_risk_write) {
      return ErrorResponse(403, "capability_not_allowed",
                           "high_risk_write policy is disabled");
    }
    const auto prefix = std::string("/api/v1/agents/");
    const auto suffix = std::string("/upgrade");
    const auto agent_id = request.path.substr(
        prefix.size(), request.path.size() - prefix.size() - suffix.size());
    const auto agent = database->FindAgent(agent_id);
    if (!agent.has_value()) {
      return ErrorResponse(404, "agent_not_found", "agent was not found");
    }
    const auto input = ParseUpgradeRequestBody(request.body);
    const auto package_id = input.package_id;
    const auto package = database->FindAgentPackage(package_id);
    if (!package.has_value()) {
      return ErrorResponse(404, "package_not_found", "package was not found");
    }
    if (package->status == "retired") {
      return ErrorResponse(409, "package_retired", "package was retired");
    }
    if (package->component != "agent") {
      return ErrorResponse(400, "package_not_agent",
                           "upgrade target must be an agent package");
    }
    const auto asset = database->FindLatestAssetSnapshot(agent_id);
    if (!asset.has_value() || package->platform != agent->platform ||
        package->arch != asset->arch) {
      return ErrorResponse(400, "platform_arch_mismatch",
                           "package target does not match agent");
    }
    if (package->version == agent->agent_version) {
      return ErrorResponse(400, "version_not_newer",
                           "upgrade target must be newer than current agent");
    }
    if (!VersionAtLeast(package->version, agent->agent_version)) {
      return ErrorResponse(400, "downgrade_not_allowed",
                           "agent downgrade is not allowed");
    }
    const auto now = zfleet::core::NowUtcRfc3339();
    const auto task_id = zfleet::core::GenerateUuid();
    const auto expires_at = input.expires_at;
    const auto manifest =
        zfleet::package::ParseManifestJson(package->manifest_json);
    const auto create_package_task =
        [&](const AgentPackageRecord& target, const std::string& id) {
          return zfleet::protocol::Task{
              .protocol_version =
                  std::string(zfleet::protocol::protocol_version()),
              .task_id = id,
              .agent_id = agent_id,
              .task_type = zfleet::protocol::TaskType::package_update,
              .capability_level =
                  zfleet::protocol::CapabilityLevel::high_risk_write,
              .created_at = now,
              .expires_at = expires_at,
              .input = zfleet::protocol::PackageUpdateInput{
                  .component = target.component,
                  .package_id = target.package_id,
                  .version = target.version,
                  .platform = target.platform,
                  .arch = target.arch,
                  .build_type = target.build_type,
                  .package_url =
                      ctx.package_download_base_url + "/api/v1/packages/" +
                      target.component + "/" + target.package_id + "/download",
                  .package_sha256 = target.sha256,
                  .manifest_sha256 =
                      zfleet::crypto::Sha256BytesHex(target.manifest_json),
                  .min_installer_version =
                      zfleet::package::ParseManifestJson(target.manifest_json)
                          .min_installer_version,
                  .allow_downgrade = false,
                  .force = false,
              },
          };
        };
    const auto task = create_package_task(*package, task_id);
    std::optional<zfleet::protocol::Task> prerequisite_task;
    if (!manifest.min_installer_version.empty()) {
      std::optional<AgentPackageRecord> installer;
      for (const auto& candidate : database->ListAgentPackages()) {
        if (candidate.component == "installer" &&
            candidate.status == "published" &&
            candidate.platform == package->platform &&
            candidate.arch == package->arch &&
            candidate.build_type == package->build_type &&
            VersionAtLeast(candidate.version, manifest.min_installer_version)) {
          if (!installer.has_value() ||
              VersionAtLeast(candidate.version, installer->version)) {
            installer = candidate;
          }
        }
      }
      if (!installer.has_value()) {
        return ErrorResponse(409, "installer_too_old",
                             "no compatible installer package is published");
      }
      prerequisite_task = create_package_task(
          *installer, zfleet::core::GenerateUuid());
    }
    database->ScheduleAgentUpgrade(agent_id, package->version, package_id,
                                   input.set_by, now, task, prerequisite_task);
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::agent_upgrade_requested,
        task_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"agent_id", agent_id},
             {"package_id", package_id},
             {"task_id", task_id},
             {"capability_level", std::string("high_risk_write")}}));
    if (prerequisite_task.has_value()) {
      RecordManagementAudit(
          database, zfleet::protocol::AuditEventType::task_queued,
          prerequisite_task->task_id, "success",
          zfleet::protocol::SerializeAuditPayload(
              {{"agent_id", agent_id},
               {"task_id", prerequisite_task->task_id},
               {"task_type", std::string("package_update")},
               {"component", std::string("installer")},
               {"next_task_id", task_id}}));
    }
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::task_queued,
        task_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"agent_id", agent_id},
             {"task_id", task_id},
             {"task_type", std::string("package_update")},
             {"package_id", package_id}}));
    return JsonResponse({{"task_id", task_id},
                         {"prerequisite_task_id",
                          prerequisite_task.has_value()
                              ? prerequisite_task->task_id
                              : std::string{}},
                         {"agent_id", agent_id},
                         {"desired_package_id", package_id},
                         {"desired_version", package->version},
                         {"upgrade_state", "queued"}},
                        201);
  }

  return std::nullopt;
}

std::optional<HttpResponse> HandleAdminPackageApi(const ApiContext& ctx) {
  const auto& request = ctx.request;
  auto* database = ctx.database;
  if (request.method == "GET" && request.path == "/api/v1/admin/packages") {
    nlohmann::json packages = nlohmann::json::array();
    for (const auto& package : database->ListAgentPackages()) {
      packages.push_back(ToJson(package));
    }
    return JsonResponse({{"packages", std::move(packages)}});
  }

  if (request.method == "POST" && request.path == "/api/v1/admin/packages") {
    return ErrorResponse(500, "package_stream_required",
                         "package upload was not streamed");
  }

  if (request.method == "POST" &&
      request.path.rfind("/api/v1/admin/packages/", 0) == 0 &&
      request.path.ends_with("/publish")) {
    const auto prefix = std::string("/api/v1/admin/packages/");
    const auto suffix = std::string("/publish");
    const auto package_id = request.path.substr(
        prefix.size(), request.path.size() - prefix.size() - suffix.size());
    const auto package = database->FindAgentPackage(package_id);
    if (!package.has_value()) {
      return ErrorResponse(404, "package_not_found", "package was not found");
    }
    const auto input = ParsePublishRequestBody(request.body);
    const auto channel = input.channel.empty() ? "stable" : input.channel;
    if (channel != "stable" && channel != "candidate" && channel != "dev") {
      return ErrorResponse(400, "channel_not_allowed",
                           "channel must be stable, candidate or dev");
    }
    if (channel == "stable" && package->build_type == "debug") {
      return ErrorResponse(400, "build_type_not_allowed",
                           "debug package cannot be published to stable");
    }
    if (package->status == "retired") {
      return ErrorResponse(409, "package_retired",
                           "retired package cannot be published");
    }
    const auto published_at = zfleet::core::NowUtcRfc3339();
    database->PublishAgentPackage(package_id, package->component, channel,
                                  package->platform, package->arch,
                                  package->build_type, input.published_by,
                                  published_at);
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::package_published,
        package_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"package_id", package_id}, {"channel", channel}}));
    const auto published = database->FindAgentPackage(package_id);
    return JsonResponse(ToJson(*published));
  }

  if (request.method == "POST" &&
      request.path.rfind("/api/v1/admin/packages/", 0) == 0 &&
      request.path.ends_with("/retire")) {
    const auto prefix = std::string("/api/v1/admin/packages/");
    const auto suffix = std::string("/retire");
    const auto package_id = request.path.substr(
        prefix.size(), request.path.size() - prefix.size() - suffix.size());
    if (!database->FindAgentPackage(package_id).has_value()) {
      return ErrorResponse(404, "package_not_found", "package was not found");
    }
    database->RetireAgentPackage(package_id, zfleet::core::NowUtcRfc3339());
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::package_retired,
        package_id, "success",
        zfleet::protocol::SerializeAuditPayload({{"package_id", package_id}}));
    return JsonResponse(ToJson(*database->FindAgentPackage(package_id)));
  }

  return std::nullopt;
}

std::optional<HttpResponse> HandlePackageDownloadApi(const ApiContext& ctx) {
  const auto& request = ctx.request;
  auto* database = ctx.database;
  if (request.method == "GET" &&
      request.path.rfind("/api/v1/packages/", 0) == 0 &&
      request.path.size() > std::string("/download").size()) {
    const auto prefix = std::string("/api/v1/packages/");
    const auto suffix = std::string("/download");
    if (request.path.ends_with(suffix)) {
      const auto rest =
          request.path.substr(prefix.size(),
                              request.path.size() - prefix.size() - suffix.size());
      const auto delimiter = rest.find('/');
      if (delimiter == std::string::npos) {
        return ErrorResponse(404, "not_found", "endpoint was not found");
      }
      const auto component = rest.substr(0, delimiter);
      const auto package_id = rest.substr(delimiter + 1);
      const auto package = database->FindAgentPackage(package_id);
      if (!package.has_value()) {
        return ErrorResponse(404, "package_not_found", "package was not found");
      }
      if (package->component != component) {
        return ErrorResponse(404, "package_not_found", "package was not found");
      }
      std::ifstream stream(package->storage_path, std::ios::binary);
      if (!stream) {
        return ErrorResponse(404, "package_file_missing",
                             "package file was not found");
      }
      std::ostringstream body;
      body << stream.rdbuf();
      return HttpResponse{
          .status = 200,
          .content_type = "application/zip",
          .body = body.str(),
      };
    }
  }

  return std::nullopt;
}

HttpResponse HandleApi(const HttpRequest& request,
                       ServerDatabase* database,
                       const std::filesystem::path& package_repository,
                       bool allow_high_risk_write,
                       const std::string& package_download_base_url,
                       const std::string& control_url,
                       const std::filesystem::path& web_static_root) {
  const auto now = zfleet::core::NowUtcRfc3339();
  for (const auto& agent_id : database->ExpireWaitingReconnect(now)) {
    RecordManagementAudit(
        database, zfleet::protocol::AuditEventType::agent_upgrade_confirmed,
        zfleet::core::GenerateUuid(), "error",
        zfleet::protocol::SerializeAuditPayload(
            {{"agent_id", agent_id},
             {"state", std::string("failed")},
             {"error_code", std::string("waiting_reconnect_timeout")}}));
  }
  if (const auto response = HandleAgentApi(ApiContext{
          .request = request,
          .database = database,
          .package_repository = package_repository,
          .allow_high_risk_write = allow_high_risk_write,
          .package_download_base_url = package_download_base_url,
          .control_url = control_url,
          .web_static_root = web_static_root,
      });
      response.has_value()) {
    return *response;
  }
  if (const auto response = HandleInstallApi(ApiContext{
          .request = request,
          .database = database,
          .package_repository = package_repository,
          .allow_high_risk_write = allow_high_risk_write,
          .package_download_base_url = package_download_base_url,
          .control_url = control_url,
          .web_static_root = web_static_root,
      });
      response.has_value()) {
    return *response;
  }
  if (const auto response = HandleAgentMutationApi(ApiContext{
          .request = request,
          .database = database,
          .package_repository = package_repository,
          .allow_high_risk_write = allow_high_risk_write,
          .package_download_base_url = package_download_base_url,
          .control_url = control_url,
          .web_static_root = web_static_root,
      });
      response.has_value()) {
    return *response;
  }
  if (const auto response = HandleAdminPackageApi(ApiContext{
          .request = request,
          .database = database,
          .package_repository = package_repository,
          .allow_high_risk_write = allow_high_risk_write,
          .package_download_base_url = package_download_base_url,
          .control_url = control_url,
          .web_static_root = web_static_root,
      });
      response.has_value()) {
    return *response;
  }
  if (const auto response = HandlePackageDownloadApi(ApiContext{
          .request = request,
          .database = database,
          .package_repository = package_repository,
          .allow_high_risk_write = allow_high_risk_write,
          .package_download_base_url = package_download_base_url,
          .control_url = control_url,
          .web_static_root = web_static_root,
      });
      response.has_value()) {
    return *response;
  }

  return ErrorResponse(404, "not_found", "endpoint was not found");
}

HttpResponse RouteRequest(const HttpRequest& request,
                          ServerDatabase* database,
                          const std::filesystem::path& package_repository,
                          const StaticFileService& static_files,
                          bool allow_high_risk_write,
                          const std::string& package_download_base_url,
                          const std::string& control_url,
                          const std::filesystem::path& web_static_root) {
  if (request.path.rfind("/api/v1/", 0) == 0) {
    return HandleApi(request, database, package_repository,
                     allow_high_risk_write, package_download_base_url,
                     control_url,
                     web_static_root);
  }
  if (request.method != "GET") {
    return ErrorResponse(405, "method_not_allowed", "method is not allowed");
  }
  const auto asset = static_files.Read(request.path);
  return HttpResponse{
      .status = asset.status,
      .content_type = asset.content_type,
      .body = asset.body,
  };
}

class ManagementHttpSession
    : public std::enable_shared_from_this<ManagementHttpSession> {
 public:
  ManagementHttpSession(tcp::socket socket,
                        ServerDatabase* database,
                        const std::filesystem::path& package_repository,
                        const StaticFileService& static_files,
                        ManagementHttpServerOptions options)
      : socket_(std::move(socket)),
        timer_(socket_.get_executor()),
        database_(database),
        package_repository_(package_repository),
        static_files_(static_files),
        options_(options),
        parser_(std::make_unique<http::request_parser<http::buffer_body>>()) {
    parser_->header_limit(options_.max_header_bytes);
    parser_->body_limit(options_.max_body_bytes);
  }

  ~ManagementHttpSession() {
    if (staging_stream_.is_open()) {
      staging_stream_.close();
    }
    if (!staging_path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove(staging_path_, ignored);
    }
  }

  void Start() {
    ArmTimeout();
    auto self = shared_from_this();
    http::async_read_header(
        socket_,
        buffer_,
        *parser_,
        [self](const boost::system::error_code& error,
               std::size_t /*bytes_transferred*/) {
          self->OnHeader(error);
        });
  }

 private:
  void ArmTimeout() {
    timer_.expires_after(options_.request_timeout);
    auto self = shared_from_this();
    timer_.async_wait([self](const boost::system::error_code& error) {
      if (!error) {
        self->Respond(
            ErrorResponse(408, "request_timeout", "request timed out"));
      }
    });
  }

  void OnHeader(const boost::system::error_code& error) {
    if (responding_) {
      return;
    }
    timer_.cancel();
    if (error == http::error::header_limit) {
      Respond(ErrorResponse(431, "headers_too_large",
                            "request headers exceed size limit"));
      return;
    }
    if (error == http::error::body_limit) {
      Respond(ErrorResponse(413, "body_too_large",
                            "request body exceeds size limit"));
      return;
    }
    if (error == http::error::end_of_stream ||
        error == boost::asio::error::operation_aborted) {
      Close();
      return;
    }
    if (error) {
      Respond(ErrorResponse(400, "invalid_request", error.message()));
      return;
    }

    request_ = ToApplicationRequest(parser_->get());
    if (request_.method == "POST" &&
        request_.path == "/api/v1/admin/packages") {
      streaming_upload_ = true;
      if (!OpenStagingFile()) {
        return;
      }
    }
    ArmTimeout();
    ReadBody();
  }

  bool OpenStagingFile() {
    package_id_ = zfleet::core::GenerateUuid();
    staging_path_ =
        package_repository_ / ".staging" / (package_id_ + ".zip");
    try {
      std::filesystem::create_directories(staging_path_.parent_path());
      staging_stream_.open(staging_path_, std::ios::binary | std::ios::trunc);
    } catch (const std::exception& ex) {
      Respond(ErrorResponse(500, "package_staging_failed", ex.what()));
      return false;
    }
    if (!staging_stream_) {
      Respond(ErrorResponse(500, "package_staging_failed",
                            "failed to open package staging file"));
      return false;
    }
    return true;
  }

  void ReadBody() {
    if (parser_->is_done()) {
      FinishBody();
      return;
    }
    parser_->get().body().data = body_buffer_.data();
    parser_->get().body().size = body_buffer_.size();
    auto self = shared_from_this();
    http::async_read_some(
        socket_,
        buffer_,
        *parser_,
        [self](const boost::system::error_code& error,
               std::size_t /*bytes_transferred*/) {
          self->OnBodyChunk(error);
        });
  }

  void OnBodyChunk(const boost::system::error_code& error) {
    if (responding_) {
      return;
    }
    const auto body_bytes =
        body_buffer_.size() - parser_->get().body().size;
    if (body_bytes != 0) {
      if (streaming_upload_) {
        staging_stream_.write(body_buffer_.data(),
                              static_cast<std::streamsize>(body_bytes));
        uploaded_bytes_ += body_bytes;
        if (!staging_stream_) {
          Respond(ErrorResponse(500, "package_staging_failed",
                                "failed to write package staging file"));
          return;
        }
      } else {
        request_.body.append(body_buffer_.data(), body_bytes);
      }
    }

    if (error == http::error::body_limit) {
      Respond(ErrorResponse(413, "body_too_large",
                            "request body exceeds size limit"));
      return;
    }
    if (error && error != http::error::need_buffer) {
      if (error == boost::asio::error::operation_aborted) {
        Close();
        return;
      }
      Respond(ErrorResponse(400, "invalid_request", error.message()));
      return;
    }
    if (parser_->is_done()) {
      FinishBody();
      return;
    }
    ReadBody();
  }

  void FinishBody() {
    timer_.cancel();
    if (streaming_upload_) {
      staging_stream_.close();
    }
    Dispatch();
  }

  void Dispatch() {
    try {
      if (streaming_upload_) {
        Respond(CommitAgentPackageUpload(request_, database_,
                                         package_repository_, staging_path_,
                                         package_id_, uploaded_bytes_));
      } else {
        Respond(RouteRequest(request_, database_, package_repository_,
                             static_files_, options_.allow_high_risk_write,
                             options_.package_download_base_url,
                             options_.control_url,
                             options_.web_static_root));
      }
    } catch (const std::exception& ex) {
      ZFLOG_ERROR(zfleet::core::log::Component("management_http"),
                  "management request failed: {}",
                  ex.what());
      Respond(ErrorResponse(500, "internal_error", ex.what()));
    }
  }

  void Respond(HttpResponse response) {
    if (responding_) {
      return;
    }
    responding_ = true;
    timer_.cancel();
    boost::system::error_code ignored;
    socket_.cancel(ignored);
    output_ = ToBeastResponse(response);
    auto self = shared_from_this();
    http::async_write(
        socket_,
        output_,
        [self](const boost::system::error_code& /*error*/,
               std::size_t /*bytes_transferred*/) {
          self->Close();
        });
  }

  void Close() {
    timer_.cancel();
    if (staging_stream_.is_open()) {
      staging_stream_.close();
    }
    if (streaming_upload_ && !staging_path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove(staging_path_, ignored);
    }
    boost::system::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

  tcp::socket socket_;
  boost::asio::steady_timer timer_;
  ServerDatabase* database_;
  const std::filesystem::path& package_repository_;
  const StaticFileService& static_files_;
  ManagementHttpServerOptions options_;
  beast::flat_buffer buffer_;
  std::unique_ptr<http::request_parser<http::buffer_body>> parser_;
  std::array<char, 8192> body_buffer_{};
  HttpRequest request_;
  http::response<http::string_body> output_;
  std::ofstream staging_stream_;
  std::filesystem::path staging_path_;
  std::string package_id_;
  std::uintmax_t uploaded_bytes_ = 0;
  bool streaming_upload_ = false;
  bool responding_ = false;
};

}  // namespace

ManagementHttpServer::ManagementHttpServer(
    std::string listen_address,
    ServerDatabase* database,
    std::filesystem::path package_repository,
    std::filesystem::path web_static_dir,
    ManagementHttpServerOptions options)
    : endpoint_(ParseListenAddress(listen_address)),
      io_context_(),
      acceptor_(io_context_),
      database_(database),
      package_repository_(std::move(package_repository)),
      static_files_(std::move(web_static_dir)),
      options_(options) {
  options_.web_static_root = static_files_.root();
}

ManagementHttpServer::~ManagementHttpServer() {
  Stop();
}

void ManagementHttpServer::Run() {
  Start();
  for (auto& thread : io_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

void ManagementHttpServer::Start() {
  if (acceptor_.is_open()) {
    return;
  }
  static_files_.ValidateRequiredFiles();
  acceptor_.open(endpoint_.protocol());
  acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
  acceptor_.bind(endpoint_);
  acceptor_.listen();
  StartAccept();

  const auto thread_count = std::max<std::size_t>(1, options_.io_threads);
  for (std::size_t i = 0; i < thread_count; ++i) {
    io_threads_.emplace_back([this] {
      io_context_.run();
    });
  }
}

void ManagementHttpServer::Stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  boost::system::error_code ignored;
  acceptor_.close(ignored);
  io_context_.stop();
  for (auto& thread : io_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

std::uint16_t ManagementHttpServer::port() const noexcept {
  if (!acceptor_.is_open()) {
    return endpoint_.port();
  }
  boost::system::error_code ignored;
  return acceptor_.local_endpoint(ignored).port();
}

void ManagementHttpServer::StartAccept() {
  acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
    if (!ec) {
      std::make_shared<ManagementHttpSession>(
          std::move(socket),
          database_,
          package_repository_,
          static_files_,
          options_)
          ->Start();
    }
    if (!stopping_) {
      StartAccept();
    }
  });
}

}  // namespace zfleet::server
