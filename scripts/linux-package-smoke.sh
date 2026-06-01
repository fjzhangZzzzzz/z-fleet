#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'USAGE'
Usage: ./scripts/linux-package-smoke.sh --arch <arch> <package.zip> [<package.zip>...]
USAGE
}

arch=""
packages=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --arch"
      arch="$2"
      shift 2
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

[[ -n "$arch" ]] || zf_fail_arg "missing --arch"
[[ ${#packages[@]} -ge 1 ]] || { usage; zf_fail_arg "missing package path"; }

for package_path in "${packages[@]}"; do
  [[ -f "$package_path" ]] || zf_fail_exec "package archive not found: $package_path"

  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-smoke.XXXXXX")"

  unzip -q "$package_path" -d "$work_dir" ||
    zf_fail_exec "failed to extract package: $package_path"

  manifest_path="$work_dir/META/manifest.json"
  [[ -f "$manifest_path" ]] || zf_fail_exec "manifest not found in package: $package_path"

  package_info="$(python3 - "$manifest_path" "$arch" <<'PY'
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1])
expected_arch = sys.argv[2]
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
assert manifest["platform"] == "linux"
assert manifest["arch"] == expected_arch
component = manifest["component"]
assert component in {"agent", "server", "installer"}
print(component)
PY
  )"

  component="$(printf '%s\n' "$package_info" | sed -n '1p')"
  binary_path="$work_dir/payload/bin/zfleet_${component}"
  [[ -f "$binary_path" ]] || zf_fail_exec "entry binary missing: $binary_path"

  [[ -x "$binary_path" ]] || zf_fail_exec "entry binary is not executable: $binary_path"
  "$binary_path" --help >/dev/null
  rm -rf "$work_dir"
done
