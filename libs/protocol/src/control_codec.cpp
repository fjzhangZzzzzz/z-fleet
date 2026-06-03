#include "zfleet/protocol/control_codec.h"

#include "zfleet/protocol/v1/agent_control.pb.h"

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace zfleet::protocol {
namespace {

namespace proto = zfleet::protocol::v1;

template <typename Message>
std::string SerializeProto(const Message& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes)) {
    throw std::runtime_error("failed to serialize protobuf message");
  }
  return bytes;
}

std::optional<TaskType> ToDomainTaskType(proto::TaskType type) {
  switch (type) {
    case proto::TASK_TYPE_COLLECT_BASIC_INVENTORY:
      return TaskType::collect_basic_inventory;
    case proto::TASK_TYPE_PACKAGE_UPDATE:
      return TaskType::package_update;
    case proto::TASK_TYPE_UNSPECIFIED:
      return std::nullopt;
  }
  return std::nullopt;
}

proto::TaskType ToProtoTaskType(TaskType type) {
  switch (type) {
    case TaskType::collect_basic_inventory:
      return proto::TASK_TYPE_COLLECT_BASIC_INVENTORY;
    case TaskType::package_update:
      return proto::TASK_TYPE_PACKAGE_UPDATE;
  }
  return proto::TASK_TYPE_UNSPECIFIED;
}

std::optional<TaskExecutionStatus> ToDomainStatus(
    proto::TaskExecutionStatus status) {
  switch (status) {
    case proto::TASK_EXECUTION_STATUS_SUCCEEDED:
      return TaskExecutionStatus::succeeded;
    case proto::TASK_EXECUTION_STATUS_FAILED:
      return TaskExecutionStatus::failed;
    case proto::TASK_EXECUTION_STATUS_EXPIRED:
      return TaskExecutionStatus::expired;
    case proto::TASK_EXECUTION_STATUS_UNSPECIFIED:
      return std::nullopt;
  }
  return std::nullopt;
}

proto::TaskExecutionStatus ToProtoStatus(TaskExecutionStatus status) {
  switch (status) {
    case TaskExecutionStatus::succeeded:
      return proto::TASK_EXECUTION_STATUS_SUCCEEDED;
    case TaskExecutionStatus::failed:
      return proto::TASK_EXECUTION_STATUS_FAILED;
    case TaskExecutionStatus::expired:
      return proto::TASK_EXECUTION_STATUS_EXPIRED;
  }
  return proto::TASK_EXECUTION_STATUS_UNSPECIFIED;
}

std::optional<CapabilityLevel> ToDomainCapabilityLevel(
    proto::CapabilityLevel level) {
  switch (level) {
    case proto::CAPABILITY_LEVEL_READONLY:
      return CapabilityLevel::readonly;
    case proto::CAPABILITY_LEVEL_LOW_RISK_WRITE:
      return CapabilityLevel::low_risk_write;
    case proto::CAPABILITY_LEVEL_HIGH_RISK_WRITE:
      return CapabilityLevel::high_risk_write;
    case proto::CAPABILITY_LEVEL_SHELL:
      return CapabilityLevel::shell;
    case proto::CAPABILITY_LEVEL_UNSPECIFIED:
      return std::nullopt;
  }
  return std::nullopt;
}

proto::CapabilityLevel ToProtoCapabilityLevel(CapabilityLevel level) {
  switch (level) {
    case CapabilityLevel::readonly:
      return proto::CAPABILITY_LEVEL_READONLY;
    case CapabilityLevel::low_risk_write:
      return proto::CAPABILITY_LEVEL_LOW_RISK_WRITE;
    case CapabilityLevel::high_risk_write:
      return proto::CAPABILITY_LEVEL_HIGH_RISK_WRITE;
    case CapabilityLevel::shell:
      return proto::CAPABILITY_LEVEL_SHELL;
  }
  return proto::CAPABILITY_LEVEL_UNSPECIFIED;
}

std::optional<ErrorCode> ToDomainErrorCode(proto::ErrorCode code) {
  switch (code) {
    case proto::ERROR_CODE_UNSUPPORTED_PROTOCOL_VERSION:
      return ErrorCode::unsupported_protocol_version;
    case proto::ERROR_CODE_AGENT_NOT_REGISTERED:
      return ErrorCode::agent_not_registered;
    case proto::ERROR_CODE_INTERNAL_ERROR:
      return ErrorCode::internal_error;
    case proto::ERROR_CODE_TASK_NOT_FOUND:
      return ErrorCode::task_not_found;
    case proto::ERROR_CODE_TASK_ALREADY_FINISHED:
      return ErrorCode::task_already_finished;
    case proto::ERROR_CODE_TASK_EXPIRED:
      return ErrorCode::task_expired;
    case proto::ERROR_CODE_UNSUPPORTED_TASK_TYPE:
      return ErrorCode::unsupported_task_type;
    case proto::ERROR_CODE_CAPABILITY_NOT_ALLOWED:
      return ErrorCode::capability_not_allowed;
    case proto::ERROR_CODE_PACKAGE_NOT_FOUND:
      return ErrorCode::package_not_found;
    case proto::ERROR_CODE_PACKAGE_RETIRED:
      return ErrorCode::package_retired;
    case proto::ERROR_CODE_PLATFORM_ARCH_MISMATCH:
      return ErrorCode::platform_arch_mismatch;
    case proto::ERROR_CODE_BUILD_TYPE_NOT_ALLOWED:
      return ErrorCode::build_type_not_allowed;
    case proto::ERROR_CODE_INSTALLER_TOO_OLD:
      return ErrorCode::installer_too_old;
    case proto::ERROR_CODE_DOWNLOAD_FAILED:
      return ErrorCode::download_failed;
    case proto::ERROR_CODE_CHECKSUM_MISMATCH:
      return ErrorCode::checksum_mismatch;
    case proto::ERROR_CODE_APPLY_FAILED:
      return ErrorCode::apply_failed;
    case proto::ERROR_CODE_START_NEW_AGENT_FAILED:
      return ErrorCode::start_new_agent_failed;
    case proto::ERROR_CODE_WAITING_RECONNECT_TIMEOUT:
      return ErrorCode::waiting_reconnect_timeout;
    case proto::ERROR_CODE_AGENT_REPORTED_UNEXPECTED_VERSION:
      return ErrorCode::agent_reported_unexpected_version;
    case proto::ERROR_CODE_UNSPECIFIED:
      return std::nullopt;
  }
  return std::nullopt;
}

proto::ErrorCode ToProtoErrorCode(ErrorCode code) {
  switch (code) {
    case ErrorCode::unsupported_protocol_version:
      return proto::ERROR_CODE_UNSUPPORTED_PROTOCOL_VERSION;
    case ErrorCode::agent_not_registered:
      return proto::ERROR_CODE_AGENT_NOT_REGISTERED;
    case ErrorCode::task_not_found:
      return proto::ERROR_CODE_TASK_NOT_FOUND;
    case ErrorCode::task_already_finished:
      return proto::ERROR_CODE_TASK_ALREADY_FINISHED;
    case ErrorCode::task_expired:
      return proto::ERROR_CODE_TASK_EXPIRED;
    case ErrorCode::unsupported_task_type:
      return proto::ERROR_CODE_UNSUPPORTED_TASK_TYPE;
    case ErrorCode::capability_not_allowed:
      return proto::ERROR_CODE_CAPABILITY_NOT_ALLOWED;
    case ErrorCode::package_not_found:
      return proto::ERROR_CODE_PACKAGE_NOT_FOUND;
    case ErrorCode::package_retired:
      return proto::ERROR_CODE_PACKAGE_RETIRED;
    case ErrorCode::platform_arch_mismatch:
      return proto::ERROR_CODE_PLATFORM_ARCH_MISMATCH;
    case ErrorCode::build_type_not_allowed:
      return proto::ERROR_CODE_BUILD_TYPE_NOT_ALLOWED;
    case ErrorCode::installer_too_old:
      return proto::ERROR_CODE_INSTALLER_TOO_OLD;
    case ErrorCode::download_failed:
      return proto::ERROR_CODE_DOWNLOAD_FAILED;
    case ErrorCode::checksum_mismatch:
      return proto::ERROR_CODE_CHECKSUM_MISMATCH;
    case ErrorCode::apply_failed:
      return proto::ERROR_CODE_APPLY_FAILED;
    case ErrorCode::start_new_agent_failed:
      return proto::ERROR_CODE_START_NEW_AGENT_FAILED;
    case ErrorCode::waiting_reconnect_timeout:
      return proto::ERROR_CODE_WAITING_RECONNECT_TIMEOUT;
    case ErrorCode::agent_reported_unexpected_version:
      return proto::ERROR_CODE_AGENT_REPORTED_UNEXPECTED_VERSION;
    case ErrorCode::invalid_json:
    case ErrorCode::missing_required_field:
    case ErrorCode::invalid_field_type:
    case ErrorCode::agent_id_mismatch:
    case ErrorCode::internal_error:
    case ErrorCode::task_agent_mismatch:
    case ErrorCode::task_execution_failed:
    case ErrorCode::task_result_invalid:
      return proto::ERROR_CODE_INTERNAL_ERROR;
  }
  return proto::ERROR_CODE_INTERNAL_ERROR;
}

PackageUpdateInput ToDomainPackageUpdateInput(
    const proto::PackageUpdateInput& input) {
  return PackageUpdateInput{
      .action = input.action().empty() ? "apply" : input.action(),
      .component = input.component(),
      .package_id = input.package_id(),
      .version = input.version(),
      .platform = input.platform(),
      .arch = input.arch(),
      .build_type = input.build_type(),
      .package_url = input.package_url(),
      .package_sha256 = input.package_sha256(),
      .manifest_sha256 = input.manifest_sha256(),
      .min_installer_version = input.min_installer_version(),
      .allow_downgrade = input.allow_downgrade(),
      .force = input.force(),
  };
}

void FillProtoPackageUpdateInput(const PackageUpdateInput& input,
                                 proto::PackageUpdateInput* output) {
  output->set_action(input.action);
  output->set_component(input.component);
  output->set_package_id(input.package_id);
  output->set_version(input.version);
  output->set_platform(input.platform);
  output->set_arch(input.arch);
  output->set_build_type(input.build_type);
  output->set_package_url(input.package_url);
  output->set_package_sha256(input.package_sha256);
  output->set_manifest_sha256(input.manifest_sha256);
  output->set_min_installer_version(input.min_installer_version);
  output->set_allow_downgrade(input.allow_downgrade);
  output->set_force(input.force);
}

void FillProtoAgentEvent(const AgentEvent& event, proto::AgentEvent* message);

proto::AgentEvent ToProtoAgentEvent(const AgentEvent& event) {
  proto::AgentEvent message;
  FillProtoAgentEvent(event, &message);
  return message;
}

void FillProtoAgentEvent(const AgentEvent& event, proto::AgentEvent* message) {
  std::visit(
      [&](const auto& payload) {
        message->set_protocol_version(payload.protocol_version);
        message->set_message_id(payload.request_id);
        message->set_agent_id(payload.agent_id);
        message->set_occurred_at(payload.occurred_at);
      },
      event.payload);

  if (const auto* payload = std::get_if<AgentRegistration>(&event.payload)) {
    auto* output = message->mutable_register_();
    output->set_agent_version(payload->agent_version);
    output->set_hostname(payload->hostname);
    output->set_os(payload->os);
    output->set_arch(payload->arch);
    if (payload->registration_token.has_value()) {
      output->set_registration_token(*payload->registration_token);
    }
  } else if (const auto* payload =
                 std::get_if<AgentHeartbeat>(&event.payload)) {
    message->mutable_heartbeat()->set_agent_version(payload->agent_version);
  } else if (const auto* payload = std::get_if<AssetSnapshot>(&event.payload)) {
    auto* output = message->mutable_asset_snapshot();
    output->set_hostname(payload->hostname);
    output->set_os(payload->os);
    if (payload->os_version.has_value()) {
      output->set_os_version(*payload->os_version);
    }
    output->set_arch(payload->arch);
    output->set_agent_version(payload->agent_version);
    for (const auto& app : payload->applications) {
      output->add_applications(app);
    }
    for (const auto& service : payload->services) {
      output->add_services(service);
    }
  } else if (const auto* payload = std::get_if<TaskRunning>(&event.payload)) {
    auto* output = message->mutable_task_running();
    output->set_task_id(payload->task_id);
    output->set_task_type(ToProtoTaskType(payload->task_type));
  } else if (const auto* payload = std::get_if<TaskResult>(&event.payload)) {
    auto* output = message->mutable_task_result();
    output->set_task_id(payload->task_id);
    output->set_task_type(ToProtoTaskType(payload->task_type));
    output->set_status(ToProtoStatus(payload->status));
    if (payload->result.has_value()) {
      if (const auto* result =
              std::get_if<CollectBasicInventoryResult>(&*payload->result)) {
        auto* inventory = output->mutable_collect_basic_inventory();
        inventory->set_hostname(result->hostname);
        inventory->set_os(result->os);
        inventory->set_arch(result->arch);
        inventory->set_agent_version(result->agent_version);
      } else if (const auto* result =
                     std::get_if<PackageUpdateResult>(&*payload->result)) {
        auto* update = output->mutable_package_update();
        update->set_component(result->component);
        update->set_package_id(result->package_id);
        update->set_version(result->version);
        update->set_state(result->state);
        update->set_error_code(result->error_code);
        update->set_error_message(result->error_message);
      }
    }
    if (payload->error.has_value()) {
      auto* error = output->mutable_error();
      error->set_code(ToProtoErrorCode(payload->error->error_code));
      error->set_message(payload->error->message);
      error->set_retryable(payload->error->retryable);
    }
  }

}

AgentEvent ToDomainAgentEvent(const proto::AgentEvent& message) {
  switch (message.payload_case()) {
    case proto::AgentEvent::kRegister: {
      const auto& payload = message.register_();
      return AgentEvent{AgentRegistration{
          .protocol_version = message.protocol_version(),
          .request_id = message.message_id(),
          .agent_id = message.agent_id(),
          .occurred_at = message.occurred_at(),
          .agent_version = payload.agent_version(),
          .hostname = payload.hostname(),
          .os = payload.os(),
          .arch = payload.arch(),
          .registration_token =
              payload.registration_token().empty()
                  ? std::optional<std::string>{}
                  : std::optional<std::string>{payload.registration_token()},
      }};
    }
    case proto::AgentEvent::kHeartbeat: {
      const auto& payload = message.heartbeat();
      return AgentEvent{AgentHeartbeat{
          .protocol_version = message.protocol_version(),
          .request_id = message.message_id(),
          .agent_id = message.agent_id(),
          .occurred_at = message.occurred_at(),
          .agent_version = payload.agent_version(),
      }};
    }
    case proto::AgentEvent::kAssetSnapshot: {
      const auto& payload = message.asset_snapshot();
      return AgentEvent{AssetSnapshot{
          .protocol_version = message.protocol_version(),
          .request_id = message.message_id(),
          .agent_id = message.agent_id(),
          .occurred_at = message.occurred_at(),
          .hostname = payload.hostname(),
          .os = payload.os(),
          .os_version = payload.os_version().empty()
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{payload.os_version()},
          .arch = payload.arch(),
          .agent_version = payload.agent_version(),
          .applications = {payload.applications().begin(),
                           payload.applications().end()},
          .services = {payload.services().begin(), payload.services().end()},
      }};
    }
    case proto::AgentEvent::kTaskRunning: {
      const auto& payload = message.task_running();
      const auto task_type = ToDomainTaskType(payload.task_type());
      if (!task_type.has_value()) {
        throw std::runtime_error("unsupported task type");
      }
      return AgentEvent{TaskRunning{
          .protocol_version = message.protocol_version(),
          .request_id = message.message_id(),
          .task_id = payload.task_id(),
          .agent_id = message.agent_id(),
          .task_type = *task_type,
          .occurred_at = message.occurred_at(),
      }};
    }
    case proto::AgentEvent::kTaskResult: {
      const auto& payload = message.task_result();
      const auto task_type = ToDomainTaskType(payload.task_type());
      const auto status = ToDomainStatus(payload.status());
      if (!task_type.has_value()) {
        throw std::runtime_error("unsupported task type");
      }
      if (!status.has_value()) {
        throw std::runtime_error("unsupported task result status");
      }
      TaskResult result{
          .protocol_version = message.protocol_version(),
          .request_id = message.message_id(),
          .task_id = payload.task_id(),
          .agent_id = message.agent_id(),
          .task_type = *task_type,
          .occurred_at = message.occurred_at(),
          .status = *status,
          .result = std::nullopt,
          .error = std::nullopt,
      };
      if (*status == TaskExecutionStatus::succeeded) {
        if (*task_type == TaskType::collect_basic_inventory) {
          result.result = CollectBasicInventoryResult{
              .hostname = payload.collect_basic_inventory().hostname(),
              .os = payload.collect_basic_inventory().os(),
              .arch = payload.collect_basic_inventory().arch(),
              .agent_version =
                  payload.collect_basic_inventory().agent_version(),
          };
        } else {
          result.result = PackageUpdateResult{
              .component = payload.package_update().component(),
              .package_id = payload.package_update().package_id(),
              .version = payload.package_update().version(),
              .state = payload.package_update().state(),
              .error_code = payload.package_update().error_code(),
              .error_message = payload.package_update().error_message(),
          };
        }
      } else {
        const auto error_code = ToDomainErrorCode(payload.error().code());
        if (!error_code.has_value()) {
          throw std::runtime_error("task error code must be set");
        }
        result.error = TaskError{
            .error_code = *error_code,
            .message = payload.error().message(),
            .retryable = payload.error().retryable(),
        };
      }
      return AgentEvent{std::move(result)};
    }
    case proto::AgentEvent::kError:
    case proto::AgentEvent::PAYLOAD_NOT_SET:
      throw std::runtime_error("unsupported agent event payload");
  }
  throw std::runtime_error("unsupported agent event payload");
}

Task ToDomainTask(const proto::ServerCommand& command) {
  const auto& payload = command.task_assigned();
  const auto task_type = ToDomainTaskType(payload.task_type());
  const auto capability_level =
      ToDomainCapabilityLevel(payload.capability_level());
  if (!task_type.has_value()) {
    throw std::runtime_error("unsupported task type");
  }
  if (!capability_level.has_value()) {
    throw std::runtime_error("unsupported capability level");
  }

  TaskInput input = CollectBasicInventoryInput{};
  switch (*task_type) {
    case TaskType::collect_basic_inventory:
      input = CollectBasicInventoryInput{};
      break;
    case TaskType::package_update:
      input = ToDomainPackageUpdateInput(payload.package_update());
      break;
  }

  return Task{
      .protocol_version = command.protocol_version(),
      .task_id = payload.task_id(),
      .agent_id = command.agent_id(),
      .task_type = *task_type,
      .capability_level = *capability_level,
      .created_at = payload.created_at(),
      .expires_at = payload.expires_at(),
      .input = std::move(input),
  };
}

ServerCommand ToDomainServerCommand(const proto::ServerCommand& message) {
  ServerCommand command{
      .protocol_version = message.protocol_version(),
      .message_id = message.message_id(),
      .correlation_id = message.correlation_id(),
      .agent_id = message.agent_id(),
      .occurred_at = message.occurred_at(),
      .payload =
          ServerError{
              .error_code = ErrorCode::internal_error,
              .message = "unsupported server command payload",
              .retryable = false,
          },
  };

  switch (message.payload_case()) {
    case proto::ServerCommand::kTaskAssigned:
      command.payload = ToDomainTask(message);
      return command;
    case proto::ServerCommand::kError: {
      const auto error_code = ToDomainErrorCode(message.error().code());
      if (!error_code.has_value()) {
        throw std::runtime_error("server error code must be set");
      }
      command.payload = ServerError{
          .error_code = *error_code,
          .message = message.error().message(),
          .retryable = message.error().retryable(),
      };
      return command;
    }
    case proto::ServerCommand::kCancelTask:
    case proto::ServerCommand::kConfigUpdate:
    case proto::ServerCommand::kPackageUpdate:
    case proto::ServerCommand::PAYLOAD_NOT_SET:
      throw std::runtime_error("unsupported server command payload");
  }
  throw std::runtime_error("unsupported server command payload");
}

proto::ServerCommand ToProtoServerCommand(const ServerCommand& command) {
  proto::ServerCommand message;
  message.set_protocol_version(command.protocol_version);
  message.set_message_id(command.message_id);
  message.set_correlation_id(command.correlation_id);
  message.set_agent_id(command.agent_id);
  message.set_occurred_at(command.occurred_at);

  if (const auto* task = std::get_if<Task>(&command.payload)) {
    auto* output = message.mutable_task_assigned();
    output->set_task_id(task->task_id);
    output->set_task_type(ToProtoTaskType(task->task_type));
    output->set_capability_level(
        ToProtoCapabilityLevel(task->capability_level));
    output->set_created_at(task->created_at);
    output->set_expires_at(task->expires_at);
    if (std::holds_alternative<CollectBasicInventoryInput>(task->input)) {
      output->mutable_collect_basic_inventory();
    } else if (const auto* input =
                   std::get_if<PackageUpdateInput>(&task->input)) {
      FillProtoPackageUpdateInput(*input, output->mutable_package_update());
    }
  } else if (const auto* error = std::get_if<ServerError>(&command.payload)) {
    auto* output = message.mutable_error();
    output->set_code(ToProtoErrorCode(error->error_code));
    output->set_message(error->message);
    output->set_retryable(error->retryable);
  }
  return message;
}

}  // namespace

std::vector<std::uint8_t> EncodeAgentEventPayload(const AgentEvent& event) {
  const auto bytes = SerializeAgentEventBlob(event);
  return {reinterpret_cast<const std::uint8_t*>(bytes.data()),
          reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size()};
}

AgentEvent DecodeAgentEventPayload(std::span<const std::uint8_t> bytes) {
  proto::AgentEvent message;
  if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
    throw std::runtime_error("invalid protobuf agent event");
  }
  return ToDomainAgentEvent(message);
}

std::vector<std::uint8_t> EncodeServerCommandPayload(
    const ServerCommand& command) {
  const auto bytes = SerializeProto(ToProtoServerCommand(command));
  return {reinterpret_cast<const std::uint8_t*>(bytes.data()),
          reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size()};
}

ServerCommand DecodeServerCommandPayload(std::span<const std::uint8_t> bytes) {
  proto::ServerCommand message;
  if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
    throw std::runtime_error("failed to parse server command");
  }
  return ToDomainServerCommand(message);
}

std::string SerializeAgentEventBlob(const AgentEvent& event) {
  proto::AgentEvent message;
  FillProtoAgentEvent(event, &message);
  return SerializeProto(message);
}

std::string SerializeTaskInputBlob(TaskType task_type, const TaskInput& input) {
  if (!TaskInputMatchesType(task_type, input)) {
    throw std::runtime_error("task input type mismatch");
  }

  switch (task_type) {
    case TaskType::collect_basic_inventory:
      return SerializeProto(proto::CollectBasicInventoryInput{});
    case TaskType::package_update: {
      proto::PackageUpdateInput message;
      FillProtoPackageUpdateInput(std::get<PackageUpdateInput>(input),
                                  &message);
      return SerializeProto(message);
    }
  }
  throw std::runtime_error("unsupported task type for stored task input");
}

TaskInput ParseTaskInputBlob(TaskType task_type,
                             const std::string& input_blob) {
  switch (task_type) {
    case TaskType::collect_basic_inventory: {
      proto::CollectBasicInventoryInput message;
      if (!message.ParseFromString(input_blob)) {
        throw std::runtime_error("failed to parse stored task input");
      }
      return CollectBasicInventoryInput{};
    }
    case TaskType::package_update: {
      proto::PackageUpdateInput message;
      if (!message.ParseFromString(input_blob)) {
        throw std::runtime_error("failed to parse stored package update input");
      }
      return ToDomainPackageUpdateInput(message);
    }
  }
  throw std::runtime_error("unsupported stored task input type");
}

std::string SerializeTaskResultDataBlob(const TaskResultData& result) {
  if (const auto* inventory =
          std::get_if<CollectBasicInventoryResult>(&result)) {
    proto::CollectBasicInventoryResult message;
    message.set_hostname(inventory->hostname);
    message.set_os(inventory->os);
    message.set_arch(inventory->arch);
    message.set_agent_version(inventory->agent_version);
    return SerializeProto(message);
  }

  const auto& update = std::get<PackageUpdateResult>(result);
  proto::PackageUpdateResult message;
  message.set_component(update.component);
  message.set_package_id(update.package_id);
  message.set_version(update.version);
  message.set_state(update.state);
  message.set_error_code(update.error_code);
  message.set_error_message(update.error_message);
  return SerializeProto(message);
}

std::string SerializeTaskErrorBlob(const TaskError& error) {
  proto::AgentError message;
  message.set_code(ToProtoErrorCode(error.error_code));
  message.set_message(error.message);
  message.set_retryable(error.retryable);
  return SerializeProto(message);
}

}  // namespace zfleet::protocol
