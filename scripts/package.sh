#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage: ./scripts/package.sh --component <agent|server|installer> --version <version> [--preset <preset>] [--output-dir <dir>] [--min-installer-version <version>] [--build|--no-build] [--force]
EOF
}

repo_root="$ZF_REPO_ROOT"
component=""
version=""
preset="$(zf_default_preset)"
output_dir="$repo_root/build/packages"
min_installer_version="0.1.0"
do_build=1
force=0

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
    --preset)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --preset"
      preset="$2"
      shift 2
      ;;
    --output-dir)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --output-dir"
      output_dir="$2"
      shift 2
      ;;
    --min-installer-version)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --min-installer-version"
      min_installer_version="$2"
      shift 2
      ;;
    --build)
      do_build=1
      shift
      ;;
    --no-build)
      do_build=0
      shift
      ;;
    --force)
      force=1
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

zf_is_safe_segment "$version" || zf_fail_arg "invalid --version; expected [A-Za-z0-9._-]+ and not . or .."
zf_is_safe_segment "$min_installer_version" || zf_fail_arg "invalid --min-installer-version; expected [A-Za-z0-9._-]+ and not . or .."

mkdir -p "$output_dir" || zf_fail_exec "failed to create output dir: $output_dir"
output_dir_abs="$(zf_make_absolute_path "$output_dir")"
binary_name="$(zf_component_binary_name "$component")"

source_binary="$repo_root/build/$preset/apps/$component/$binary_name"
packager_binary_name="zfleet_packager"
if zf_is_windows_host; then
  packager_binary_name="${packager_binary_name}.exe"
fi
packager_binary="$repo_root/build/$preset/apps/packager/$packager_binary_name"
source_binary_arg="$(zf_to_native_path_if_needed "$source_binary")"
output_dir_arg="$(zf_to_native_path_if_needed "$output_dir_abs")"
packager_binary_arg="$(zf_to_native_path_if_needed "$packager_binary")"

if [[ $do_build -eq 1 ]]; then
  zf_log "building preset: $preset"
  "$repo_root/scripts/build.sh" "$preset" >&2 ||
    zf_fail_exec "build failed for preset: $preset"
fi

[[ -f "$source_binary" ]] || zf_fail_exec "built binary not found: $source_binary"
[[ -f "$packager_binary" ]] || zf_fail_exec "packager binary not found: $packager_binary"

packager_args=(
  pack-dir
  --component "$component"
  --version "$version"
  --binary "$source_binary_arg"
  --output-dir "$output_dir_arg"
  --min-installer-version "$min_installer_version"
)
if [[ $force -eq 1 ]]; then
  packager_args+=(--force)
fi

set +e
packager_output="$("$packager_binary_arg" "${packager_args[@]}")"
packager_exit=$?
set -e
if [[ $packager_exit -ne 0 ]]; then
  exit "$packager_exit"
fi

package_dir="$(printf '%s\n' "$packager_output" | tail -n 1)"
package_dir="${package_dir%$'\r'}"
[[ -n "$package_dir" ]] || zf_fail_exec "packager did not report package path"
package_dir="$(zf_to_posix_path_if_needed "$package_dir")"

printf '%s\n' "$package_dir"
