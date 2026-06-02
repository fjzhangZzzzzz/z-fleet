#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'USAGE'
Usage:
  ./scripts/vcpkg.sh bootstrap
  ./scripts/vcpkg.sh exec <vcpkg-args...>

Project defaults:
  tool root:     .tools/vcpkg
  installed dir: build/vcpkg_installed/<triplet>
  binary cache:  .cache/vcpkg/archives

CMake manifest mode installs dependencies during configure; use scripts/build.sh
for normal builds.
USAGE
}

zf_vcpkg_root_posix() {
  printf '%s/.tools/vcpkg\n' "$ZF_REPO_ROOT"
}

zf_vcpkg_requested_triplet() {
  local explicit_triplet="${VCPKG_TARGET_TRIPLET:-}"
  local arg
  local expect_value=0
  for arg in "$@"; do
    if [[ "$expect_value" -eq 1 ]]; then
      printf '%s\n' "$arg"
      return 0
    fi
    case "$arg" in
      --triplet=*)
        printf '%s\n' "${arg#--triplet=}"
        return 0
        ;;
      --triplet)
        expect_value=1
        ;;
    esac
  done

  if [[ -n "$explicit_triplet" ]]; then
    printf '%s\n' "$explicit_triplet"
  else
    zf_vcpkg_default_triplet
  fi
}

zf_vcpkg_installed_dir_posix() {
  local triplet="${1:-$(zf_vcpkg_requested_triplet)}"
  printf '%s/build/vcpkg_installed/%s\n' "$ZF_REPO_ROOT" "$triplet"
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

zf_vcpkg_bootstrap_bin_posix() {
  local vcpkg_dir="$1"
  if zf_is_windows_host; then
    printf '%s/vcpkg.exe\n' "$vcpkg_dir"
  else
    printf '%s/vcpkg\n' "$vcpkg_dir"
  fi
}

zf_vcpkg_tool_release_tag() {
  local vcpkg_dir="$1"
  local metadata_file="$vcpkg_dir/scripts/vcpkg-tool-metadata.txt"
  [[ -f "$metadata_file" ]] || return 1

  local release_tag
  release_tag="$(sed -n 's/^VCPKG_TOOL_RELEASE_TAG=//p' "$metadata_file" | tr -d '[:space:]')"
  [[ -n "$release_tag" ]] || return 1
  printf '%s\n' "$release_tag"
}

zf_vcpkg_should_skip_bootstrap() {
  local vcpkg_dir="$1"
  [[ "${ZF_FORCE_VCPKG_BOOTSTRAP:-0}" != "1" ]] || return 1

  local tool_bin
  local release_tag
  local version_output
  tool_bin="$(zf_vcpkg_bootstrap_bin_posix "$vcpkg_dir")"
  [[ -x "$tool_bin" ]] || return 1

  release_tag="$(zf_vcpkg_tool_release_tag "$vcpkg_dir")" || return 1
  version_output="$("$tool_bin" version --disable-metrics 2>/dev/null || true)"
  [[ "$version_output" == *"$release_tag"* ]] || return 1

  return 0
}

zf_vcpkg_checkout_matches_ref() {
  local vcpkg_dir="$1"
  local vcpkg_ref="$2"
  [[ -d "$vcpkg_dir/.git" ]] || return 1

  local current_ref
  local target_ref
  current_ref="$(git -C "$vcpkg_dir" rev-parse --verify HEAD 2>/dev/null)" ||
    return 1
  target_ref="$(git -C "$vcpkg_dir" rev-parse --verify "$vcpkg_ref^{commit}" 2>/dev/null)" ||
    return 1

  [[ "$current_ref" == "$target_ref" ]]
}

zf_vcpkg_is_shallow_repository() {
  local vcpkg_dir="$1"
  [[ -d "$vcpkg_dir/.git" ]] || return 1

  local is_shallow
  is_shallow="$(git -C "$vcpkg_dir" rev-parse --is-shallow-repository 2>/dev/null ||
    printf 'false')"
  [[ "$is_shallow" == "true" ]]
}

zf_vcpkg_is_ready() {
  local vcpkg_dir="$1"
  local vcpkg_ref="$2"

  zf_vcpkg_checkout_matches_ref "$vcpkg_dir" "$vcpkg_ref" &&
    ! zf_vcpkg_is_shallow_repository "$vcpkg_dir" &&
    zf_vcpkg_should_skip_bootstrap "$vcpkg_dir"
}

zf_validate_build_jobs() {
  local jobs="$1"
  [[ "$jobs" =~ ^[1-9][0-9]*$ ]] ||
    zf_fail_arg "jobs must be a positive integer: $jobs"
}

zf_apply_build_jobs() {
  local jobs="${1:-${ZF_BUILD_JOBS:-4}}"

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

  export VCPKG_ROOT="$vcpkg_root"
  export VCPKG_DISABLE_METRICS=1

  if zf_vcpkg_is_ready "$vcpkg_dir" "$vcpkg_ref"; then
    printf 'Reusing pinned vcpkg checkout and tool binary.\n'
    printf 'VCPKG_ROOT=%s\n' "$VCPKG_ROOT"
    return 0
  fi

  if [[ ! -d "$vcpkg_dir/.git" ]]; then
    git init "$vcpkg_dir"
    git -C "$vcpkg_dir" remote add origin https://github.com/microsoft/vcpkg.git
  fi

  if zf_vcpkg_is_shallow_repository "$vcpkg_dir"; then
    git -C "$vcpkg_dir" fetch --unshallow origin
  fi

  if ! git -C "$vcpkg_dir" rev-parse --verify "$vcpkg_ref^{commit}" >/dev/null 2>&1; then
    git -C "$vcpkg_dir" fetch origin "$vcpkg_ref"
  fi

  if ! zf_vcpkg_checkout_matches_ref "$vcpkg_dir" "$vcpkg_ref"; then
    git -C "$vcpkg_dir" checkout --detach "$vcpkg_ref"
  fi

  if zf_vcpkg_should_skip_bootstrap "$vcpkg_dir"; then
    printf 'Reusing existing vcpkg tool binary (set ZF_FORCE_VCPKG_BOOTSTRAP=1 to force re-bootstrap).\n'
  else
    if zf_is_windows_host; then
      "$vcpkg_root/bootstrap-vcpkg.bat" -disableMetrics
    else
      "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
    fi
  fi

  printf 'VCPKG_ROOT=%s\n' "$VCPKG_ROOT"
}

zf_vcpkg_setup_project_env() {
  local vcpkg_dir
  local cache_dir
  local triplet="${1:-$(zf_vcpkg_requested_triplet)}"
  local installed_dir
  vcpkg_dir="$(zf_vcpkg_root_posix)"
  cache_dir="$(zf_vcpkg_binary_cache_dir_posix)"
  installed_dir="$(zf_vcpkg_installed_dir_posix "$triplet")"

  mkdir -p "$cache_dir"

  export VCPKG_ROOT
  VCPKG_ROOT="$(zf_to_native_path_if_needed "$vcpkg_dir")"
  export VCPKG_TARGET_TRIPLET
  VCPKG_TARGET_TRIPLET="$triplet"
  export VCPKG_INSTALLED_DIR
  VCPKG_INSTALLED_DIR="$(zf_to_native_path_if_needed "$installed_dir")"
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

cmd_bootstrap() {
  [[ "$#" -eq 0 ]] || zf_fail_arg "bootstrap does not accept arguments"
  zf_vcpkg_bootstrap
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
