#pragma once

#include "installer.h"
#include "manifest.h"

#include <string>
#include <string_view>

namespace zfleet::installer {

Manifest ParseManifestJson(std::string_view manifest_json);
std::string SerializeStatusResult(const StatusResult& result);

} // namespace zfleet::installer
