#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'USAGE'
Usage:
  ./scripts/vcpkg.sh bootstrap
  eval "$(./scripts/vcpkg.sh env)"
  ./scripts/vcpkg.sh list [filter]
  ./scripts/vcpkg.sh install [--jobs <n>] [vcpkg-install-args...]
  ./scripts/vcpkg.sh exec <vcpkg-args...>

Project defaults:
  tool root:     .tools/vcpkg
  installed dir: build/vcpkg_installed
  binary cache:  .cache/vcpkg/archives

Concurrency:
  --jobs <n> or ZF_BUILD_JOBS=<n> limits vcpkg build concurrency.
USAGE
}

zf_vcpkg_root_posix() {
  printf '%s/.tools/vcpkg\n' "$ZF_REPO_ROOT"
}

zf_vcpkg_installed_dir_posix() {
  printf '%s/build/vcpkg_installed\n' "$ZF_REPO_ROOT"
}

zf_vcpkg_binary_cache_dir_posix() {
  printf '%s/.cache/vcpkg/archives\n' "$ZF_REPO_ROOT"
}

zf_vcpkg_default_triplet() {
  if zf_is_windows_host; then
    printf 'x64-windows\n'
  else
    printf 'x64-linux\n'
  fi
}

zf_vcpkg_exe() {
  printf '%s/vcpkg\n' "$VCPKG_ROOT"
}

zf_validate_build_jobs() {
  local jobs="$1"
  [[ "$jobs" =~ ^[1-9][0-9]*$ ]] ||
    zf_fail_arg "jobs must be a positive integer: $jobs"
}

zf_apply_build_jobs() {
  local jobs="${1:-${ZF_BUILD_JOBS:-}}"
  [[ -n "$jobs" ]] || return 0

  zf_validate_build_jobs "$jobs"
  export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
  export VCPKG_MAX_CONCURRENCY="$jobs"
}

zf_vcpkg_bootstrap() {
  local vcpkg_ref_file="$ZF_REPO_ROOT/.vcpkg-version"
  [[ -f "$vcpkg_ref_file" ]] || zf_fail_exec "missing $vcpkg_ref_file"

  local vcpkg_ref
  local vcpkg_dir
  local vcpkg_root
  vcpkg_ref="$(tr -d '[:space:]' < "$vcpkg_ref_file")"
  vcpkg_dir="$(zf_vcpkg_root_posix)"
  vcpkg_root="$(zf_to_native_path_if_needed "$vcpkg_dir")"

  mkdir -p "$(dirname "$vcpkg_dir")"

  if [[ ! -d "$vcpkg_dir/.git" ]]; then
    git init "$vcpkg_dir"
    git -C "$vcpkg_dir" remote add origin https://github.com/microsoft/vcpkg.git
  fi

  if ! git -C "$vcpkg_dir" rev-parse --verify "$vcpkg_ref^{commit}" >/dev/null 2>&1; then
    git -C "$vcpkg_dir" fetch --depth 1 origin "$vcpkg_ref"
  fi

  git -C "$vcpkg_dir" checkout --detach "$vcpkg_ref"

  export VCPKG_ROOT="$vcpkg_root"
  export VCPKG_DISABLE_METRICS=1

  if zf_is_windows_host; then
    "$vcpkg_root/bootstrap-vcpkg.bat" -disableMetrics
  else
    "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
  fi

  printf 'VCPKG_ROOT=%s\n' "$VCPKG_ROOT"
}

zf_vcpkg_setup_project_env() {
  local vcpkg_dir
  local cache_dir
  vcpkg_dir="$(zf_vcpkg_root_posix)"
  cache_dir="$(zf_vcpkg_binary_cache_dir_posix)"

  mkdir -p "$cache_dir"

  export VCPKG_ROOT
  VCPKG_ROOT="$(zf_to_native_path_if_needed "$vcpkg_dir")"
  export VCPKG_DISABLE_METRICS=1
  export VCPKG_BINARY_SOURCES
  VCPKG_BINARY_SOURCES="clear;files,$(zf_to_native_path_if_needed "$cache_dir"),readwrite"
}

zf_vcpkg_has_triplet_arg() {
  local arg
  local expect_value=0
  for arg in "$@"; do
    if [[ "$expect_value" -eq 1 ]]; then
      return 0
    fi
    case "$arg" in
      --triplet=*)
        return 0
        ;;
      --triplet)
        expect_value=1
        ;;
    esac
  done
  return 1
}

zf_vcpkg_run() {
  local vcpkg_bin
  vcpkg_bin="$(zf_vcpkg_exe)"
  (cd "$ZF_REPO_ROOT" && "$vcpkg_bin" "$@")
}

cmd_env() {
  [[ "$#" -eq 0 ]] || zf_fail_arg "env does not accept arguments"

  local vcpkg_root
  local cache_dir
  mkdir -p "$(zf_vcpkg_binary_cache_dir_posix)"

  vcpkg_root="$(zf_to_native_path_if_needed "$(zf_vcpkg_root_posix)")"
  cache_dir="$(zf_to_native_path_if_needed "$(zf_vcpkg_binary_cache_dir_posix)")"

  printf 'export VCPKG_ROOT=%q\n' "$vcpkg_root"
  printf 'export VCPKG_DISABLE_METRICS=%q\n' '1'
  printf 'export VCPKG_BINARY_SOURCES=%q\n' "clear;files,$cache_dir,readwrite"
}

cmd_bootstrap() {
  [[ "$#" -eq 0 ]] || zf_fail_arg "bootstrap does not accept arguments"
  zf_vcpkg_bootstrap
}

cmd_list() {
  zf_vcpkg_bootstrap
  zf_vcpkg_setup_project_env

  local installed_dir
  installed_dir="$(zf_to_native_path_if_needed "$(zf_vcpkg_installed_dir_posix)")"
  zf_vcpkg_run list --x-install-root="$installed_dir" "$@"
}

cmd_install() {
  local jobs="${ZF_BUILD_JOBS:-}"
  local args=()

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
      --)
        shift
        args+=("$@")
        break
        ;;
      *)
        args+=("$1")
        ;;
    esac
    shift
  done

  zf_apply_build_jobs "$jobs"
  zf_vcpkg_bootstrap
  zf_vcpkg_setup_project_env

  local installed_dir
  installed_dir="$(zf_to_native_path_if_needed "$(zf_vcpkg_installed_dir_posix)")"

  if ! zf_vcpkg_has_triplet_arg "${args[@]}"; then
    args=(--triplet "$(zf_vcpkg_default_triplet)" "${args[@]}")
  fi

  zf_vcpkg_run install --x-install-root="$installed_dir" "${args[@]}"
}

cmd_exec() {
  [[ "$#" -gt 0 ]] || zf_fail_arg "exec requires vcpkg arguments"
  zf_vcpkg_bootstrap
  zf_vcpkg_setup_project_env
  zf_vcpkg_run "$@"
}

main() {
  local command="${1:-}"
  [[ -n "$command" ]] || zf_fail_arg "missing command"
  shift

  case "$command" in
    bootstrap)
      cmd_bootstrap "$@"
      ;;
    env)
      cmd_env "$@"
      ;;
    list)
      cmd_list "$@"
      ;;
    install)
      cmd_install "$@"
      ;;
    exec)
      cmd_exec "$@"
      ;;
    -h|--help|help)
      usage
      ;;
    *)
      zf_fail_arg "unknown command: $command"
      ;;
  esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
