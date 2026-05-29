#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage: ./scripts/inspect-release-packages.sh --platform <linux|windows> --arch <arch> --build-type <debug|release> [--allow-partial] <package.zip> [<package.zip>...]
EOF
}

platform=
arch=
build_type=
allow_partial=0
packages=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --platform"
      platform="$2"
      shift 2
      ;;
    --arch)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --arch"
      arch="$2"
      shift 2
      ;;
    --build-type)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --build-type"
      build_type="$2"
      shift 2
      ;;
    --allow-partial)
      allow_partial=1
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
      packages+=("$1")
      shift
      ;;
  esac
done

[[ -n "$platform" ]] || zf_fail_arg "missing --platform"
[[ -n "$arch" ]] || zf_fail_arg "missing --arch"
[[ -n "$build_type" ]] || zf_fail_arg "missing --build-type"
[[ ${#packages[@]} -ge 1 ]] || zf_fail_arg "missing package path"

declare -A package_by_component
suffix=
if [[ "$platform" == "windows" ]]; then
  suffix=".exe"
fi

bootstrap_dir="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-release-bootstrap.XXXXXX")"
install_root="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-release-root.XXXXXX")"
cleanup() {
  rm -rf "$bootstrap_dir" "$install_root"
}
trap cleanup EXIT

for package_path in "${packages[@]}"; do
  [[ -f "$package_path" ]] || zf_fail_exec "package archive not found: $package_path"
  metadata="$(
    python3 - "$package_path" "$platform" "$arch" "$build_type" "$suffix" <<'PY'
import json
import pathlib
import sys
import zipfile

package = pathlib.Path(sys.argv[1])
platform = sys.argv[2]
arch = sys.argv[3]
build_type = sys.argv[4]
suffix = sys.argv[5]

with zipfile.ZipFile(package) as zf:
    manifest = json.loads(zf.read("META/manifest.json"))
    names = set(zf.namelist())
    assert manifest["platform"] == platform
    assert manifest["arch"] == arch
    assert manifest["build_type"] == build_type
    component = manifest["component"]
    if component not in {"agent", "server", "installer"}:
        raise AssertionError(f"unexpected component: {component}")
    if component == "installer":
        path = f"payload/bin/zfleet_launcher{suffix}"
        if path not in names:
            raise AssertionError(f"installer package missing launcher asset: {path}")
        if not any(file["target"] == f"bin/zfleet_launcher{suffix}" for file in manifest["files"]):
            raise AssertionError("manifest missing launcher asset target")
    print(component)
PY
  )" || zf_fail_exec "package inspection failed: $package_path"

  component="$(printf '%s\n' "$metadata" | sed -n '1p')"
  [[ -n "$component" ]] ||
    zf_fail_exec "failed to parse package metadata: $package_path"
  package_by_component["$component"]="$package_path"

  if [[ "$component" == "installer" ]]; then
    python3 - "$package_path" "$bootstrap_dir" <<'PY'
import pathlib
import sys
import zipfile

package = pathlib.Path(sys.argv[1])
output_dir = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(package) as zf:
    zf.extractall(output_dir)
PY
    bootstrap_binary="$bootstrap_dir/payload/bin/$(zf_component_binary_name installer)"
    [[ -f "$bootstrap_binary" ]] ||
      zf_fail_exec "bootstrap installer binary missing: $bootstrap_binary"
    if [[ "$platform" != "windows" ]]; then
      chmod +x "$bootstrap_binary"
      [[ -x "$bootstrap_binary" ]] ||
        zf_fail_exec "bootstrap installer binary is not executable: $bootstrap_binary"
      "$bootstrap_binary" --help >/dev/null
    else
      "$bootstrap_binary" --help >/dev/null
    fi
  fi
done

if [[ $allow_partial -eq 0 ]]; then
  for component in installer agent server; do
    [[ -n "${package_by_component[$component]:-}" ]] ||
      zf_fail_exec "missing $component package in arguments"
  done
fi

if [[ -z "${package_by_component[installer]:-}" ]]; then
  [[ $allow_partial -eq 1 ]] || zf_fail_exec "missing installer package in arguments"
  exit 0
fi

bootstrap_binary="$bootstrap_dir/payload/bin/$(zf_component_binary_name installer)"
"$bootstrap_binary" apply --root "$install_root" --package "${package_by_component[installer]}"

installed_installer="$install_root/installer/bin/$(zf_component_binary_name installer)"
[[ -f "$installed_installer" ]] ||
  zf_fail_exec "installed installer stub missing: $installed_installer"
if [[ "$platform" != "windows" ]]; then
  [[ -x "$installed_installer" ]] ||
    zf_fail_exec "installed installer stub is not executable: $installed_installer"
fi

if [[ $allow_partial -eq 1 ]]; then
  exit 0
fi

for component in agent server; do
  "$installed_installer" apply --root "$install_root" --package "${package_by_component[$component]}"
  stub_path="$install_root/$component/bin/$(zf_component_binary_name "$component")"
  [[ -f "$stub_path" ]] || zf_fail_exec "installed stub missing: $stub_path"
  if [[ "$platform" != "windows" ]]; then
    [[ -x "$stub_path" ]] || zf_fail_exec "installed stub is not executable: $stub_path"
  fi
done
