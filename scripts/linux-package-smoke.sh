#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'USAGE'
Usage: ./scripts/linux-package-smoke.sh <package.zip> [<package.zip>...]
USAGE
}

[[ $# -ge 1 ]] || { usage; zf_fail_arg "missing package path"; }

for package_path in "$@"; do
  [[ -f "$package_path" ]] || zf_fail_exec "package archive not found: $package_path"

  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-smoke.XXXXXX")"

  unzip -q "$package_path" -d "$work_dir" ||
    zf_fail_exec "failed to extract package: $package_path"

  manifest_path="$work_dir/META/manifest.json"
  [[ -f "$manifest_path" ]] || zf_fail_exec "manifest not found in package: $package_path"

  entry_path="$(python3 - "$manifest_path" <<'PY'
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1])
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
assert manifest["platform"] == "linux"
assert manifest["arch"] == "x86_64"
print(manifest["entry"])
PY
  )"

  binary_path="$work_dir/payload/$entry_path"
  [[ -f "$binary_path" ]] || zf_fail_exec "entry binary missing: $binary_path"

  [[ -x "$binary_path" ]] || zf_fail_exec "entry binary is not executable: $binary_path"
  "$binary_path" --help >/dev/null
  rm -rf "$work_dir"
done
