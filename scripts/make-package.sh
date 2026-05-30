#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage: ./scripts/make-package.sh <agent|server|installer>... [--preset <preset>] [--out <dir>] [--no-build] [--force] [--zip]
EOF
}

repo_root="$ZF_REPO_ROOT"
components=()
preset="$(zf_default_preset)"
output_dir="$repo_root/build/packages"
min_installer_version="0.1.0"
do_build=1
force=0
zip=0

copy_linux_runtime_libs() {
  local source_binary="$1"
  local payload_dir="$2"
  local lib_dir="$payload_dir/lib"
  mkdir -p "$lib_dir" || zf_fail_exec "failed to create payload lib dir"

  command -v ldd >/dev/null 2>&1 || zf_fail_exec "ldd not found in PATH"

  mapfile -t runtime_libs < <(
    ldd "$source_binary" 2>/dev/null |
      awk '
        /=> \// { print $3 }
        /^\// { print $1 }
      ' |
      sed '/^$/d' |
      sort -u
  )

  local copied=0
  for lib_path in "${runtime_libs[@]}"; do
    [[ -f "$lib_path" ]] || continue
    local lib_name
    lib_name="$(basename "$lib_path")"
    case "$lib_name" in
      libatomic.so*|libstdc++.so*|libgcc_s.so*)
        cp -p "$lib_path" "$lib_dir/$lib_name" ||
          zf_fail_exec "failed to stage runtime library: $lib_path"
        copied=1
        ;;
    esac
  done

  if [[ $copied -eq 0 ]]; then
    zf_log "warning: no Linux runtime libs matched libatomic/libstdc++/libgcc_s for $source_binary"
  fi
}

copy_windows_runtime_dlls() {
  local payload_dir="$1"
  shift
  local payload_bin_dir="$payload_dir/bin"
  mkdir -p "$payload_bin_dir" || zf_fail_exec "failed to create payload bin dir"

  local copied=0
  local source_dir dll_path dll_name
  shopt -s nullglob
  for source_dir in "$@"; do
    [[ -d "$source_dir" ]] || continue
    for dll_path in "$source_dir"/*.dll; do
      dll_name="$(basename "$dll_path")"
      cp -p "$dll_path" "$payload_bin_dir/$dll_name" ||
        zf_fail_exec "failed to stage runtime dll: $dll_path"
      copied=1
    done
  done
  shopt -u nullglob

  if [[ $copied -eq 0 ]]; then
    zf_log "warning: no runtime dll found for source directories: $*"
  fi
}

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
      components+=("$1")
      shift
      ;;
  esac
done

[[ ${#components[@]} -ge 1 ]] || zf_fail_arg "missing component"
for component in "${components[@]}"; do
  zf_validate_component "$component" ||
    zf_fail_arg "invalid component: $component; expected $(zf_known_components_description)"
done
mkdir -p "$output_dir" || zf_fail_exec "failed to create output dir: $output_dir"
output_dir_abs="$(zf_make_absolute_path "$output_dir")"
case "$preset" in
  linux-debug)
    package_platform="linux"
    package_arch="x86_64"
    package_build_type="debug"
    ;;
  linux-release)
    package_platform="linux"
    package_arch="x86_64"
    package_build_type="release"
    ;;
  windows-debug)
    package_platform="windows"
    package_arch="x86_64"
    package_build_type="debug"
    ;;
  windows-release)
    package_platform="windows"
    package_arch="x86_64"
    package_build_type="release"
    ;;
  *)
    zf_fail_arg "cannot infer target platform and architecture from preset: $preset"
    ;;
esac

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

staging_root="$(mktemp -d "${TMPDIR:-/tmp}/zfleet-package-staging.XXXXXX")" ||
  zf_fail_exec "failed to create staging directory"
cleanup_staging() {
  rm -rf "$staging_root"
}
trap cleanup_staging EXIT

[[ -f "$packager_binary" ]] || zf_fail_exec "packager binary not found: $packager_binary"

for component in "${components[@]}"; do
  binary_name="$(zf_component_binary_name "$component")"
  source_binary="$repo_root/build/$preset/apps/$component/$binary_name"
  version_file="$repo_root/build/$preset/apps/$component/zfleet_${component}_version.txt"
  [[ -f "$source_binary" ]] || zf_fail_exec "built binary not found: $source_binary"
  [[ -f "$version_file" ]] ||
    zf_fail_exec "component version file not found: $version_file; run ./scripts/build.sh $preset"

  version="$(tr -d '[:space:]' < "$version_file")"
  zf_is_safe_segment "$version" ||
    zf_fail_exec "invalid component version in $version_file: $version"

  payload_dir="$staging_root/$component/payload"
  entry_path="bin/$binary_name"
  mkdir -p "$payload_dir/bin" ||
    zf_fail_exec "failed to create payload staging dir"
  cp -p "$source_binary" "$payload_dir/$entry_path" ||
    zf_fail_exec "failed to stage component binary"
  if [[ "$component" == "installer" ]]; then
    launcher_source="$repo_root/build/$preset/apps/installer/launcher/$(zf_launcher_binary_name)"
    [[ -f "$launcher_source" ]] ||
      zf_fail_exec "launcher asset not found: $launcher_source"
    cp -p "$launcher_source" "$payload_dir/bin/$(zf_launcher_binary_name)" ||
      zf_fail_exec "failed to stage launcher asset: $launcher_source"
  fi
  if [[ "$component" == "server" ]]; then
    web_source_dir="$repo_root/apps/server/web"
    [[ -d "$web_source_dir" ]] ||
      zf_fail_exec "server Web static asset directory not found: $web_source_dir"
    mkdir -p "$payload_dir/share" ||
      zf_fail_exec "failed to create server Web asset staging dir"
    cp -Rp "$web_source_dir" "$payload_dir/share/web" ||
      zf_fail_exec "failed to stage server Web static assets"
  fi

  if [[ "$package_platform" == "linux" ]]; then
    copy_linux_runtime_libs "$source_binary" "$payload_dir"
  elif [[ "$package_platform" == "windows" ]]; then
    runtime_dirs=("$(dirname "$source_binary")")
    if [[ "$component" == "installer" ]]; then
      runtime_dirs+=("$(dirname "$launcher_source")")
    fi
    copy_windows_runtime_dlls "$payload_dir" "${runtime_dirs[@]}"
  fi
  payload_dir_arg="$(zf_to_native_path_if_needed "$payload_dir")"

  packager_args=(
    pack
    --component "$component"
    --version "$version"
    --platform "$package_platform"
    --arch "$package_arch"
    --build-type "$package_build_type"
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
done
