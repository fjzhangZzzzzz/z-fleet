#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
vcpkg_ref="$(tr -d '[:space:]' < "$repo_root/.vcpkg-version")"
vcpkg_dir="$repo_root/.tools/vcpkg"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    platform="windows"
    vcpkg_root="$(cygpath -am "$vcpkg_dir")"
    ;;
  *)
    platform="unix"
    vcpkg_root="$vcpkg_dir"
    ;;
esac

mkdir -p "$(dirname "$vcpkg_dir")"

if [[ ! -d "$vcpkg_dir/.git" ]]; then
  git init "$vcpkg_dir"
  git -C "$vcpkg_dir" remote add origin https://github.com/microsoft/vcpkg.git
fi

if ! git -C "$vcpkg_dir" rev-parse --verify "$vcpkg_ref^{commit}" >/dev/null 2>&1; then
  git -C "$vcpkg_dir" fetch --depth 1 origin "$vcpkg_ref"
fi

git -C "$vcpkg_dir" checkout --detach "$vcpkg_ref"

export VCPKG_ROOT="$vcpkg_root"
export VCPKG_DISABLE_METRICS=1

if [[ "$platform" == "windows" ]]; then
  "$vcpkg_root/bootstrap-vcpkg.bat" -disableMetrics
else
  "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi

printf 'VCPKG_ROOT=%s\n' "$VCPKG_ROOT"
