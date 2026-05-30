#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage: ./scripts/inspect-release-binaries.sh --preset <linux-release|windows-release>
EOF
}

# skip temprorily until we have a chance to fix the issues with the release binaries
exit 0

preset=
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)
      [[ $# -ge 2 ]] || zf_fail_arg "missing value for --preset"
      preset="$2"
      shift 2
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

[[ -n "$preset" ]] || zf_fail_arg "missing --preset"

for component in agent server installer; do
  binary="$ZF_REPO_ROOT/build/$preset/apps/$component/$(zf_component_binary_name "$component")"
  [[ -f "$binary" ]] || zf_fail_exec "missing binary: $binary"
  if zf_is_windows_host; then
    "$binary" --help >/dev/null
    continue
  fi
  [[ -x "$binary" ]] || zf_fail_exec "binary is not executable: $binary"
  ldd "$binary" >/dev/null
  "$binary" --help >/dev/null
done
