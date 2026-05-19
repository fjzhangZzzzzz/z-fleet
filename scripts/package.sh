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
package_dir="$output_dir_abs/$component/$version"
binary_name="$(zf_component_binary_name "$component")"

source_binary="$repo_root/build/$preset/apps/$component/$binary_name"
manifest_path="$package_dir/META/manifest.json"
payload_binary="$package_dir/payload/bin/$binary_name"

if [[ $do_build -eq 1 ]]; then
  zf_log "building preset: $preset"
  "$repo_root/scripts/build.sh" "$preset" >&2 ||
    zf_fail_exec "build failed for preset: $preset"
fi

[[ -f "$source_binary" ]] || zf_fail_exec "built binary not found: $source_binary"

if [[ -e "$package_dir" ]]; then
  if [[ $force -eq 1 ]]; then
    zf_log "removing existing package dir: $package_dir"
    rm -rf "$package_dir" || zf_fail_exec "failed to remove existing package dir: $package_dir"
  else
    zf_fail_exec "package already exists: $package_dir (use --force to overwrite)"
  fi
fi

zf_log "creating package dir: $package_dir"
mkdir -p "$package_dir/META" "$package_dir/payload/bin" || zf_fail_exec "failed to create package dir"
cp -p "$source_binary" "$payload_binary" || zf_fail_exec "failed to copy binary into package"

size_bytes="$(wc -c < "$payload_binary" | tr -d '[:space:]')"
sha256_value="$(zf_compute_sha256 "$payload_binary")"

cat > "$manifest_path" <<EOF
{
  "schema_version": 1,
  "component": "$component",
  "version": "$version",
  "min_installer_version": "$min_installer_version",
  "files": [
    {
      "source": "payload/bin/$binary_name",
      "target": "bin/$binary_name",
      "size": $size_bytes,
      "sha256": "$sha256_value",
      "executable": true
    }
  ],
  "signatures": []
}
EOF

printf '%s\n' "$package_dir"
