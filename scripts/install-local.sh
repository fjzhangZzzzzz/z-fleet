#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage:
  ./scripts/install-local.sh apply <agent|server|installer> [--root <root>] [--preset <preset>] [--zip] [--force] [--no-build]
  ./scripts/install-local.sh status <agent|server|installer> [--root <root>] [--preset <preset>]
  ./scripts/install-local.sh rollback <agent|server|installer> [--root <root>] [--preset <preset>]
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

repo_root="$ZF_REPO_ROOT"
[[ $# -ge 1 ]] || zf_fail_arg "missing subcommand"
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 2
fi

command_name="$1"
shift

root="/tmp/zfleet-root"
preset="$(zf_default_preset)"
packages_dir="$repo_root/build/packages"
component=""
force=0
do_build=1
no_build_requested=0
zip=0

while [[ $# -gt 0 ]]; do
  case "$1" in
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
    --zip)
      zip=1
      shift
      ;;
    --force)
      force=1
      shift
      ;;
    --no-build)
      do_build=0
      no_build_requested=1
      shift
      ;;
    -h|--help)
      usage
      exit 2
      ;;
    --*)
      zf_fail_arg "unknown argument: $1"
      ;;
    *)
      if [[ -n "$component" ]]; then
        zf_fail_arg "unexpected argument: $1"
      fi
      component="$1"
      shift
      ;;
  esac
done

zf_validate_component "$component" ||
  zf_fail_arg "invalid --component; expected $(zf_known_components_description)"

root_abs="$(zf_make_absolute_path "$root")"

case "$command_name" in
  apply)
    package_args=(
      "$component"
      --preset "$preset"
      --out "$packages_dir"
    )
    if [[ $zip -eq 1 ]]; then
      package_args+=(--zip)
    fi
    if [[ $force -eq 1 ]]; then
      package_args+=(--force)
    fi
    if [[ $do_build -eq 0 ]]; then
      package_args+=(--no-build)
    fi

    zf_log "generating package for $component"
    package_output="$("$repo_root/scripts/make-package.sh" "${package_args[@]}")" ||
      zf_fail_exec "package generation failed"
    package_path_abs="$(printf '%s\n' "$package_output" | tail -n 1)"
    package_path_abs="${package_path_abs%$'\r'}"

    if [[ $zip -eq 1 ]]; then
      [[ -f "$package_path_abs" ]] || zf_fail_exec "package archive not found: $package_path_abs"
      [[ "$package_path_abs" == *.zip ]] ||
        zf_fail_exec "package archive must use .zip extension: $package_path_abs"
    else
      [[ -d "$package_path_abs" ]] || zf_fail_exec "package dir not found: $package_path_abs"
    fi

    installer_binary="$repo_root/build/$preset/apps/installer/$(zf_component_binary_name installer)"

    [[ -f "$installer_binary" ]] || zf_fail_exec "installer build output not found: $installer_binary"

    root_arg="$(zf_to_native_path_if_needed "$root_abs")"
    package_arg="$(zf_to_native_path_if_needed "$package_path_abs")"

    zf_log "applying package with installer"
    run_installer "$installer_binary" apply --root "$root_arg" --package "$package_arg" ||
      zf_fail_exec "installer apply failed"

    zf_log "copying launcher stub"
    copy_launcher_stub "$component" "$preset" "$root_abs"
    ;;
  status)
    [[ $zip -eq 0 ]] || zf_fail_arg "--zip is not supported for status"
    [[ $force -eq 0 ]] || zf_fail_arg "--force is not supported for status"
    [[ $no_build_requested -eq 0 ]] || zf_fail_arg "--no-build is not supported for status"
    installer_binary="$(resolve_installer_binary "$root_abs" "$preset")"
    root_arg="$(zf_to_native_path_if_needed "$root_abs")"
    run_installer "$installer_binary" status --root "$root_arg" --component "$component"
    ;;
  rollback)
    [[ $zip -eq 0 ]] || zf_fail_arg "--zip is not supported for rollback"
    [[ $force -eq 0 ]] || zf_fail_arg "--force is not supported for rollback"
    [[ $no_build_requested -eq 0 ]] || zf_fail_arg "--no-build is not supported for rollback"
    installer_binary="$(resolve_installer_binary "$root_abs" "$preset")"
    root_arg="$(zf_to_native_path_if_needed "$root_abs")"
    run_installer "$installer_binary" rollback --root "$root_arg" --component "$component" ||
      zf_fail_exec "installer rollback failed"
    ;;
  *)
    zf_fail_arg "unknown subcommand: $command_name"
    ;;
esac
