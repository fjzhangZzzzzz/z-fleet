#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

script_dir="$ZF_SCRIPT_DIR"
repo_root="$ZF_REPO_ROOT"
preset="${1:-$(zf_default_preset)}"

source "$script_dir/bootstrap-vcpkg.sh"

cache_dir="$repo_root/.cache/vcpkg/archives"
mkdir -p "$cache_dir"

cache_dir="$(zf_to_native_path_if_needed "$cache_dir")"

export VCPKG_BINARY_SOURCES="clear;files,$cache_dir,readwrite"

cmake --preset "$preset"
cmake --build --preset "$preset"
