#include "zfleet/transport/http2_session.h"
#include "zfleet/transport/nghttp2_compat.h"

namespace zfleet::transport {

std::uint32_t LinkedNghttp2Version() {
  const auto* info = nghttp2_version(0);
  return info == nullptr ? 0 : info->version_num;
}

std::string_view Http2ErrorMessage(std::uint32_t error_code) noexcept {
  return nghttp2_http2_strerror(error_code);
}

} // namespace zfleet::transport
