#include "zfleet/core/version.h"

int main() {
  return zfleet::core::project_name().empty() || zfleet::core::version().empty();
}
