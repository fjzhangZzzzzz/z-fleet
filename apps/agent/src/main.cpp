#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <iostream>

int main() {
  std::cout << zfleet::core::project_name() << " agent "
            << zfleet::core::version() << " protocol "
            << zfleet::protocol::protocol_version() << " on "
            << zfleet::platform::os_name() << '\n';
  return 0;
}
