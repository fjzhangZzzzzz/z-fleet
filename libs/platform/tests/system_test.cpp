#include "zfleet/platform/system.h"

int main() {
  return zfleet::platform::os_name().empty() ||
         zfleet::platform::architecture_name().empty() ||
         zfleet::platform::hostname().empty();
}
