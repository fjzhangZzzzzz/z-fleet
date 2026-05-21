#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

preset="${1:-$(zf_default_preset)}"

ctest --preset "$preset"
