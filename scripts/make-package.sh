#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage: ./scripts/make-package.sh <agent|server|installer> [--preset <preset>] [--out <dir>] [--no-build] [--force] [--zip]
EOF
}

repo_root="$ZF_REPO_ROOT"
component=""
preset="$(zf_default_preset)"
output_dir="$repo_root/build/packages"
min_installer_version="0.1.0"
do_build=1
force=0
zip=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --preset"
      preset="$2"
      shift 2
      ;;
    --out)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --out"
      output_dir="$2"
      shift 2
      ;;
    --no-build)
      do_build=0
      shift
      ;;
    --force)
      force=1
      shift
      ;;
    --zip)
      zip=1
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
mkdir -p "$output_dir" || zf_fail_exec "failed to create output dir: $output_dir"
output_dir_abs="$(zf_make_absolute_path "$output_dir")"
binary_name="$(zf_component_binary_name "$component")"
case "$preset" in
  linux-*)
    package_platform="linux"
    package_arch="x86_64"
    ;;
  windows-*)
    package_platform="windows"
    package_arch="x86_64"
    ;;
  *)
    zf_fail_arg "cannot infer target platform and architecture from preset: $preset"
    ;;
esac

source_binary="$repo_root/build/$preset/apps/$component/$binary_name"
version_file="$repo_root/build/$preset/apps/$component/zfleet_${component}_version.txt"
packager_binary_name="zfleet_packager"
if zf_is_windows_host; then
  packager_binary_name="${packager_binary_name}.exe"
fi
packager_binary="$repo_root/build/$preset/apps/packager/$packager_binary_name"
output_dir_arg="$(zf_to_native_path_if_needed "$output_dir_abs")"
packager_binary_arg="$(zf_to_native_path_if_needed "$packager_binary")"

if [[ $do_build -eq 1 ]]; then
  zf_log "building preset: $preset"
  "$repo_root/scripts/build.sh" "$preset" >&2 ||
    zf_fail_exec "build failed for preset: $preset"
fi

[[ -f "$source_binary" ]] || zf_fail_exec "built binary not found: $source_binary"
[[ -f "$version_file" ]] ||
  zf_fail_exec "component version file not found: $version_file; run ./scripts/build.sh $preset"
[[ -f "$packager_binary" ]] || zf_fail_exec "packager binary not found: $packager_binary"

version="$(tr -d '[:space:]' < "$version_file")"
zf_is_safe_segment "$version" ||
  zf_fail_exec "invalid component version in $version_file: $version"

staging_root="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-package-staging.XXXXXX")" ||
  zf_fail_exec "failed to create staging directory"
cleanup_staging() {
  rm -rf "$staging_root"
}
trap cleanup_staging EXIT

payload_dir="$staging_root/payload"
entry_path="bin/$binary_name"
mkdir -p "$payload_dir/bin" || zf_fail_exec "failed to create payload staging dir"
cp -p "$source_binary" "$payload_dir/$entry_path" ||
  zf_fail_exec "failed to stage component binary"
if [[ "$component" == "server" ]]; then
  web_source_dir="$repo_root/apps/server/web"
  [[ -d "$web_source_dir" ]] ||
    zf_fail_exec "server Web static asset directory not found: $web_source_dir"
  mkdir -p "$payload_dir/share" ||
    zf_fail_exec "failed to create server Web asset staging dir"
  cp -Rp "$web_source_dir" "$payload_dir/share/web" ||
    zf_fail_exec "failed to stage server Web static assets"
fi
payload_dir_arg="$(zf_to_native_path_if_needed "$payload_dir")"

packager_args=()
packager_args+=(
  pack
  --component "$component"
  --version "$version"
  --platform "$package_platform"
  --arch "$package_arch"
  --payload-dir "$payload_dir_arg"
  --entry "$entry_path"
  --output-dir "$output_dir_arg"
  --min-installer-version "$min_installer_version"
)
if [[ $zip -eq 1 ]]; then
  packager_args+=(--zip)
fi
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

package_path="$(printf '%s\n' "$packager_output" | tail -n 1)"
package_path="${package_path%$'\r'}"
[[ -n "$package_path" ]] || zf_fail_exec "packager did not report package path"
package_path="$(zf_to_posix_path_if_needed "$package_path")"

printf '%s\n' "$package_path"
