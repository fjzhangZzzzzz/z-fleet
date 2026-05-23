#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>

namespace zfleet::protocol {

using AuditPayloadValue = std::variant<std::string, bool, std::int64_t>;

struct AuditPayloadField {
  std::string_view key;
  AuditPayloadValue value;
};

std::string SerializeAuditPayload(
    std::initializer_list<AuditPayloadField> fields);

} // namespace zfleet::protocol
