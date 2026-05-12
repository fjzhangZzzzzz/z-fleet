#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
preset="${1:-}"

if [[ -z "$preset" ]]; then
  echo "Usage: $0 <cmake-preset>" >&2
  exit 2
fi

source "$script_dir/bootstrap-vcpkg.sh"

cache_dir="$repo_root/.cache/vcpkg/archives"
mkdir -p "$cache_dir"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    cache_dir="$(cygpath -am "$cache_dir")"
    ;;
esac

export VCPKG_BINARY_SOURCES="clear;files,$cache_dir,readwrite"

cmake --preset "$preset"
cmake --build --preset "$preset"
