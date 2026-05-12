#include "zfleet/platform/system.h"

int main() {
  return zfleet::platform::os_name().empty();
}
