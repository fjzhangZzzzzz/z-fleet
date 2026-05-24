#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

preset="${1:-$(zf_default_preset)}"

zf_run_in_msvc_env ctest --preset "$preset"
