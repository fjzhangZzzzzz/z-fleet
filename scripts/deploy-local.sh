#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage:
  ./scripts/deploy-local.sh apply --component <agent|server|installer> --version <version> [--root <root>] [--preset <preset>] [--packages-dir <dir>] [--package-dir <dir>] [--force-package] [--build|--no-build]
  ./scripts/deploy-local.sh status --component <component> [--root <root>] [--preset <preset>]
  ./scripts/deploy-local.sh rollback --component <component> [--root <root>] [--preset <preset>]
EOF
}

copy_launcher_stub() {
  local component="$1"
  local preset="$2"
  local root="$3"
  local binary_name
  binary_name="$(zf_component_binary_name "$component")"

  local launcher_source="$repo_root/build/$preset/apps/launcher/$binary_name"
  local launcher_target="$root/zfleet/$component/bin/$binary_name"
  [[ -f "$launcher_source" ]] || zf_fail_exec "launcher stub not found: $launcher_source"

  mkdir -p "$(dirname "$launcher_target")" || zf_fail_exec "failed to create launcher target dir"
  cp -p "$launcher_source" "$launcher_target" || zf_fail_exec "failed to copy launcher stub"
}

resolve_installer_binary() {
  local root="$1"
  local preset="$2"
  local binary_name
  binary_name="$(zf_component_binary_name installer)"

  local deployed="$root/zfleet/installer/bin/$binary_name"
  local build_output="$repo_root/build/$preset/apps/installer/$binary_name"
  if [[ -f "$deployed" ]]; then
    printf '%s\n' "$deployed"
  else
    printf '%s\n' "$build_output"
  fi
}

run_installer() {
  local installer_path="$1"
  shift

  if [[ ! -f "$installer_path" ]]; then
    zf_fail_exec "installer binary not found: $installer_path"
  fi

  set +e
  "$installer_path" "$@"
  local exit_code=$?
  set -e
  return "$exit_code"
}

validate_package_identity() {
  local package_path="$1"
  local expected_component="$2"
  local expected_version="$3"
  local manifest_path="$package_path/META/manifest.json"

  [[ -f "$manifest_path" ]] ||
    zf_fail_exec "package manifest not found: $manifest_path"
  grep -Eq "\"component\"[[:space:]]*:[[:space:]]*\"$expected_component\"" \
    "$manifest_path" ||
    zf_fail_exec "package component does not match --component: $manifest_path"
  grep -Eq "\"version\"[[:space:]]*:[[:space:]]*\"$expected_version\"" \
    "$manifest_path" ||
    zf_fail_exec "package version does not match --version: $manifest_path"
}

repo_root="$ZF_REPO_ROOT"
[[ $# -ge 1 ]] || zf_fail_arg "missing subcommand"

command_name="$1"
shift

root="/tmp/zfleet-root"
preset="$(zf_default_preset)"
packages_dir="$repo_root/build/packages"
package_dir=""
component=""
version=""
force_package=0
do_build=1
package_generated=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --component)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --component"
      component="$2"
      shift 2
      ;;
    --version)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --version"
      version="$2"
      shift 2
      ;;
    --root)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --root"
      root="$2"
      shift 2
      ;;
    --preset)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --preset"
      preset="$2"
      shift 2
      ;;
    --packages-dir)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --packages-dir"
      packages_dir="$2"
      shift 2
      ;;
    --package-dir)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --package-dir"
      package_dir="$2"
      shift 2
      ;;
    --force-package)
      force_package=1
      shift
      ;;
    --build)
      do_build=1
      shift
      ;;
    --no-build)
      do_build=0
      shift
      ;;
    -h|--help)
      usage
      exit 2
      ;;
    *)
      zf_fail_arg "unknown argument: $1"
      ;;
  esac
done

zf_validate_component "$component" ||
  zf_fail_arg "invalid --component; expected agent, server, or installer"

root_abs="$(zf_make_absolute_path "$root")"

case "$command_name" in
  apply)
    zf_is_safe_segment "$version" || zf_fail_arg "invalid --version; expected [A-Za-z0-9._-]+ and not . or .."

    if [[ -n "$package_dir" ]]; then
      package_dir_abs="$(zf_make_absolute_path "$package_dir")"
    else
      package_args=(
        --component "$component"
        --version "$version"
        --preset "$preset"
        --output-dir "$packages_dir"
      )
      if [[ $force_package -eq 1 ]]; then
        package_args+=(--force)
      fi
      if [[ $do_build -eq 1 ]]; then
        package_args+=(--build)
      else
        package_args+=(--no-build)
      fi

      zf_log "generating package for $component $version"
      package_output="$("$repo_root/scripts/package.sh" "${package_args[@]}")" ||
        zf_fail_exec "package generation failed"
      package_dir_abs="$(printf '%s\n' "$package_output" | tail -n 1)"
      package_generated=1
    fi

    [[ -d "$package_dir_abs" ]] || zf_fail_exec "package dir not found: $package_dir_abs"
    validate_package_identity "$package_dir_abs" "$component" "$version"

    installer_binary="$repo_root/build/$preset/apps/installer/$(zf_component_binary_name installer)"

    if [[ $do_build -eq 1 && $package_generated -eq 0 ]]; then
      zf_log "building preset for apply: $preset"
      "$repo_root/scripts/build.sh" "$preset" || zf_fail_exec "build failed for preset: $preset"
    fi

    [[ -f "$installer_binary" ]] || zf_fail_exec "installer build output not found: $installer_binary"

    root_arg="$(zf_to_native_path_if_needed "$root_abs")"
    package_arg="$(zf_to_native_path_if_needed "$package_dir_abs")"

    zf_log "applying package with installer"
    run_installer "$installer_binary" apply --root "$root_arg" --package "$package_arg" ||
      zf_fail_exec "installer apply failed"

    zf_log "copying launcher stub"
    copy_launcher_stub "$component" "$preset" "$root_abs"
    ;;
  status)
    [[ -z "$version" ]] || zf_fail_arg "--version is not supported for status"
    installer_binary="$(resolve_installer_binary "$root_abs" "$preset")"
    root_arg="$(zf_to_native_path_if_needed "$root_abs")"
    run_installer "$installer_binary" status --root "$root_arg" --component "$component"
    ;;
  rollback)
    [[ -z "$version" ]] || zf_fail_arg "--version is not supported for rollback"
    installer_binary="$(resolve_installer_binary "$root_abs" "$preset")"
    root_arg="$(zf_to_native_path_if_needed "$root_abs")"
    run_installer "$installer_binary" rollback --root "$root_arg" --component "$component" ||
      zf_fail_exec "installer rollback failed"
    ;;
  *)
    zf_fail_arg "unknown subcommand: $command_name"
    ;;
esac
