#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/vcpkg.sh"

usage() {
  cat >&2 <<'USAGE'
Usage:
  ./scripts/build.sh [preset] [--jobs <n>]

Options:
  --jobs <n>  Limit CMake and vcpkg build concurrency.

Environment:
  ZF_BUILD_JOBS=<n>  Default jobs value when --jobs is not specified.
USAGE
}

preset=""
jobs="${ZF_BUILD_JOBS:-}"

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --jobs)
      shift
      [[ "$#" -gt 0 ]] || zf_fail_arg "missing value for --jobs"
      jobs="$1"
      ;;
    --jobs=*)
      jobs="${1#--jobs=}"
      [[ -n "$jobs" ]] || zf_fail_arg "missing value for --jobs"
      ;;
    -h|--help|help)
      usage
      exit 0
      ;;
    --)
      shift
      [[ "$#" -le 1 ]] || zf_fail_arg "expected at most one preset"
      if [[ "$#" -eq 1 ]]; then
        [[ -z "$preset" ]] || zf_fail_arg "preset specified more than once"
        preset="$1"
      fi
      break
      ;;
    -*)
      zf_fail_arg "unknown option: $1"
      ;;
    *)
      [[ -z "$preset" ]] || zf_fail_arg "preset specified more than once"
      preset="$1"
      ;;
  esac
  shift
done

preset="${preset:-$(zf_default_preset)}"
zf_preset_triplet "$preset" >/dev/null ||
  zf_fail_arg "unsupported preset: $preset"

zf_apply_build_jobs "$jobs"
zf_vcpkg_bootstrap
mkdir -p "$(zf_vcpkg_binary_cache_dir_posix)"
export VCPKG_DISABLE_METRICS=1

zf_run_in_msvc_env cmake --preset "$preset"
zf_run_in_msvc_env cmake --build --preset "$preset"
