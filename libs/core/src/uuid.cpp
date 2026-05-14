#include "zfleet/core/uuid.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace zfleet::core {

std::string GenerateUuid() {
  return boost::uuids::to_string(boost::uuids::random_generator()());
}

} // namespace zfleet::core
