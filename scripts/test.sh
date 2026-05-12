#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
preset="${1:-}"

if [[ -z "$preset" ]]; then
  echo "Usage: $0 <cmake-preset>" >&2
  exit 2
fi

source "$script_dir/bootstrap-vcpkg.sh"

ctest --preset "$preset"
