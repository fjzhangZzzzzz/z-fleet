#!/usr/bin/env bash

zf_log() {
  printf '%s\n' "$*" >&2
}

zf_fail_exec() {
  zf_log "$1"
  exit 1
}

zf_fail_arg() {
  zf_log "$1"
  if declare -F usage >/dev/null 2>&1; then
    usage
  fi
  exit 2
}

zf_is_windows_host() {
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

zf_default_preset() {
  if zf_is_windows_host; then
    printf 'windows-debug\n'
  else
    printf 'linux-debug\n'
  fi
}

zf_is_safe_segment() {
  local value="$1"
  [[ -n "$value" ]] &&
    [[ "$value" != "." ]] &&
    [[ "$value" != ".." ]] &&
    [[ "$value" =~ ^[A-Za-z0-9._-]+$ ]]
}

zf_make_absolute_path() {
  local path="$1"
  if [[ "$path" = /* ]]; then
    printf '%s\n' "$path"
    return 0
  fi

  local dir_name
  local base_name
  dir_name="$(dirname "$path")"
  base_name="$(basename "$path")"
  (
    cd "$dir_name"
    printf '%s/%s\n' "$(pwd -P)" "$base_name"
  )
}

zf_to_native_path_if_needed() {
  local path="$1"
  if zf_is_windows_host && command -v cygpath >/dev/null 2>&1; then
    cygpath -am "$path"
  else
    printf '%s\n' "$path"
  fi
}

zf_to_windows_path_if_needed() {
  local path="$1"
  if zf_is_windows_host && command -v cygpath >/dev/null 2>&1; then
    cygpath -aw "$path"
  else
    printf '%s\n' "$path"
  fi
}

zf_to_posix_path_if_needed() {
  local path="$1"
  if zf_is_windows_host && command -v cygpath >/dev/null 2>&1; then
    cygpath -au "$path"
  else
    printf '%s\n' "$path"
  fi
}

zf_to_posix_path_list_if_needed() {
  local path="$1"
  if zf_is_windows_host && command -v cygpath >/dev/null 2>&1; then
    cygpath -aup "$path"
  else
    printf '%s\n' "$path"
  fi
}

zf_vswhere_path() {
  printf '%s\n' '/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
}

zf_find_vs_devcmd() {
  [[ -f "$(zf_vswhere_path)" ]] || return 1

  local install_path
  install_path="$(
    "$(zf_vswhere_path)" \
      -latest \
      -products '*' \
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
      -property installationPath 2>/dev/null | tr -d '\r'
  )"
  [[ -n "$install_path" ]] || return 1
  install_path="$(zf_to_posix_path_if_needed "$install_path")"

  local devcmd_path="$install_path/Common7/Tools/VsDevCmd.bat"
  if [[ ! -f "$devcmd_path" ]]; then
    devcmd_path="$install_path/Common7/Tools/LaunchDevCmd.bat"
  fi
  [[ -f "$devcmd_path" ]] || return 1

  printf '%s\n' "$devcmd_path"
}

zf_run_in_msvc_env() {
  if ! zf_is_windows_host; then
    "$@"
    return $?
  fi

  local devcmd_path
  devcmd_path="$(zf_find_vs_devcmd)" ||
    zf_fail_exec "Visual Studio Build Tools not found; expected VsDevCmd.bat or LaunchDevCmd.bat"

  local devcmd_native
  devcmd_native="$(zf_to_windows_path_if_needed "$devcmd_path")"

  local env_script
  env_script="$(mktemp "${TMPDIR:-/tmp}/zfleet-vsdevcmd.XXXXXX.cmd")" ||
    zf_fail_exec "failed to create temporary Visual Studio environment script"
  cat > "$env_script" <<EOF
@echo off
call "$devcmd_native" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
%*
EOF

  local exit_code=0
  "$env_script" "$@" || exit_code=$?
  rm -f "$env_script"
  return "$exit_code"
}

zf_component_binary_name() {
  local component="$1"
  local binary_name="zfleet_${component}"
  if zf_is_windows_host; then
    binary_name="${binary_name}.exe"
  fi
  printf '%s\n' "$binary_name"
}

zf_known_components_description() {
  printf 'agent, server, or installer\n'
}

zf_validate_component() {
  case "$1" in
    agent|server|installer)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

zf_compute_sha256() {
  local file_path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$file_path" | awk '{print $1}'
    return 0
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$file_path" | awk '{print $1}'
    return 0
  fi
  zf_fail_exec "sha256 tool not found; expected sha256sum or shasum"
}

_zf_source_index=$((${#BASH_SOURCE[@]} - 1))
_zf_caller_source="${BASH_SOURCE[$_zf_source_index]}"
ZF_SCRIPT_DIR="$(cd "$(dirname "$_zf_caller_source")" && pwd)"
ZF_REPO_ROOT="$(cd "$ZF_SCRIPT_DIR/.." && pwd)"
unset _zf_source_index
unset _zf_caller_source
