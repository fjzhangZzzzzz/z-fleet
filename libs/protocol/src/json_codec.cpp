#include "zfleet/protocol/json_codec.h"

#include <nlohmann/json.hpp>

#include <string>

namespace zfleet::protocol {

std::string SerializeAuditPayload(
    std::initializer_list<AuditPayloadField> fields) {
  nlohmann::json payload = nlohmann::json::object();
  for (const auto& field : fields) {
    std::visit(
        [&](const auto& value) {
          payload[std::string(field.key)] = value;
        },
        field.value);
  }
  return payload.dump();
}

} // namespace zfleet::protocol
