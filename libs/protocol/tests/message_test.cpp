#include "zfleet/protocol/message.h"

int main() {
  using zfleet::protocol::MessageKind;

  return zfleet::protocol::protocol_version().empty() ||
         zfleet::protocol::to_string(MessageKind::heartbeat) != "heartbeat";
}
