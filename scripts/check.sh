#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage:
  ./scripts/check.sh binaries --preset <linux-release|windows-release>
  ./scripts/check.sh packages --platform <linux|windows> --arch <arch> --build-type <debug|release> [--allow-partial] <package.zip> [<package.zip>...]
EOF
}

check_binaries() {
  local preset=
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
}

check_packages() {
  local platform=
  local arch=
  local build_type=
  local allow_partial=0
  local packages=()

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

  local python_cmd=
  for cmd in python3 python; do
    if command -v "$cmd" >/dev/null 2>&1 && "$cmd" -c 'import json, zipfile' >/dev/null 2>&1; then
      python_cmd="$cmd"
      break
    fi
  done
  [[ -n "$python_cmd" ]] || zf_fail_exec "python runtime unavailable: require a working 'python3' or 'python' in PATH"

  declare -A package_by_component
  local suffix=
  if [[ "$platform" == "windows" ]]; then
    suffix=".exe"
  fi

  bootstrap_dir=
  install_root=
  bootstrap_dir="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-release-bootstrap.XXXXXX")"
  install_root="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-release-root.XXXXXX")"
  trap 'rm -rf "$bootstrap_dir" "$install_root"' EXIT

  for package_path in "${packages[@]}"; do
    [[ -f "$package_path" ]] || zf_fail_exec "package archive not found: $package_path"
    local metadata
    if ! metadata="$(
      "$python_cmd" - "$package_path" "$platform" "$arch" "$build_type" "$suffix" 2>&1 <<'PY'
import json
import pathlib
import sys
import zipfile

package = pathlib.Path(sys.argv[1])
platform = sys.argv[2]
arch = sys.argv[3]
build_type = sys.argv[4]
suffix = sys.argv[5]

def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(2)

try:
    with zipfile.ZipFile(package) as zf:
        try:
            manifest = json.loads(zf.read("META/manifest.json"))
        except KeyError as exc:
            fail(f"missing manifest file in archive: {exc}")
        except json.JSONDecodeError as exc:
            fail(f"manifest JSON is invalid: {exc}")

        names = set(zf.namelist())

        if manifest.get("platform") != platform:
            fail(f"manifest platform mismatch: expected '{platform}', got '{manifest.get('platform')}'")
        if manifest.get("arch") != arch:
            fail(f"manifest arch mismatch: expected '{arch}', got '{manifest.get('arch')}'")
        if manifest.get("build_type") != build_type:
            fail(f"manifest build_type mismatch: expected '{build_type}', got '{manifest.get('build_type')}'")

        component = manifest.get("component")
        if component not in {"agent", "server", "installer"}:
            fail(f"unexpected component: {component}")

        if component == "installer":
            path = f"payload/bin/zfleet_launcher{suffix}"
            if path not in names:
                fail(f"installer package missing launcher asset: {path}")
            if not any(file.get("target") == f"bin/zfleet_launcher{suffix}" for file in manifest.get("files", [])):
                fail("manifest missing launcher asset target")

        print(component)
except zipfile.BadZipFile:
    fail(f"invalid zip archive: {package}")
PY
    )"; then
      zf_fail_exec "package inspection failed: $package_path: ${metadata:-unknown error}"
    fi

    local component
    component="$(printf '%s\n' "$metadata" | sed -n '1p')"
    [[ -n "$component" ]] ||
      zf_fail_exec "failed to parse package metadata: $package_path"
    package_by_component["$component"]="$package_path"

    if [[ "$component" == "installer" ]]; then
      "$python_cmd" - "$package_path" "$bootstrap_dir" <<'PY'
import pathlib
import sys
import zipfile

package = pathlib.Path(sys.argv[1])
output_dir = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(package) as zf:
    zf.extractall(output_dir)
PY
      local bootstrap_binary="$bootstrap_dir/payload/bin/$(zf_component_binary_name installer)"
      [[ -f "$bootstrap_binary" ]] ||
        zf_fail_exec "bootstrap installer binary missing: $bootstrap_binary"
      if [[ "$platform" != "windows" ]]; then
        chmod +x "$bootstrap_binary"
        [[ -x "$bootstrap_binary" ]] ||
          zf_fail_exec "bootstrap installer binary is not executable: $bootstrap_binary"
        "$bootstrap_binary" --help >/dev/null ||
          zf_fail_exec "bootstrap installer --help failed: $bootstrap_binary"
      else
        "$bootstrap_binary" --help >/dev/null ||
          zf_fail_exec "bootstrap installer --help failed: $bootstrap_binary"
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

  local bootstrap_binary="$bootstrap_dir/payload/bin/$(zf_component_binary_name installer)"
  "$bootstrap_binary" apply --root "$install_root" --package "${package_by_component[installer]}" ||
    zf_fail_exec "installer apply failed for package: ${package_by_component[installer]}"

  local installed_installer="$install_root/installer/bin/$(zf_component_binary_name installer)"
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
    "$installed_installer" apply --root "$install_root" --package "${package_by_component[$component]}" ||
      zf_fail_exec "installer apply failed for $component package: ${package_by_component[$component]}"
    local stub_path="$install_root/$component/bin/$(zf_component_binary_name "$component")"
    [[ -f "$stub_path" ]] || zf_fail_exec "installed stub missing: $stub_path"
    if [[ "$platform" != "windows" ]]; then
      [[ -x "$stub_path" ]] || zf_fail_exec "installed stub is not executable: $stub_path"
    fi
  done
}

main() {
  local command="${1:-}"
  if [[ -z "$command" ]]; then
    usage
    exit 2
  fi
  shift

  case "$command" in
    binaries)
      check_binaries "$@"
      ;;
    packages)
      check_packages "$@"
      ;;
    -h|--help)
      usage
      exit 2
      ;;
    *)
      zf_fail_arg "unknown command: $command"
      ;;
  esac
}

main "$@"
