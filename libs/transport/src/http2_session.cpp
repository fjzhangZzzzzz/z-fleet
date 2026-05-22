#include "zfleet/transport/http2_session.h"

#include <nghttp2/nghttp2.h>

namespace zfleet::transport {

std::uint32_t LinkedNghttp2Version() {
  const auto* info = nghttp2_version(0);
  return info == nullptr ? 0 : info->version_num;
}

} // namespace zfleet::transport
