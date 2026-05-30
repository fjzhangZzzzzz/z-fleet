#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"
source "$ZF_SCRIPT_DIR/lib/build_container.sh"

usage() {
  cat >&2 <<'USAGE'
Usage:
  ./scripts/builder.sh [preset] [--jobs <n>] [--test]
  ./scripts/builder.sh --shell [--root]

Runs commands in the Rocky Linux builder container.

Modes:
  (default)  Run ./scripts/build.sh, optionally followed by ./scripts/test.sh.
  --shell    Start an interactive shell instead of building.

Options:
  --jobs <n>  Limit CMake and vcpkg build concurrency (build mode only).
  --test      Run ./scripts/test.sh after a successful build (build mode only).
  --root      Run the container as root instead of the current user (shell mode).

Environment:
  ZF_BUILDER_IMAGE            Override the full builder image reference.
  ZF_BUILD_CONTAINER_WORKDIR  Override the mounted workdir (default: pwd).
  ZF_BUILD_JOBS=<n>           Default jobs value when --jobs is not specified.

Proxy env vars set on the host (for example HTTP_PROXY, HTTPS_PROXY, NO_PROXY)
are detected automatically and forwarded into the container. When proxy is
detected, OPENSSL_CONF is set to scripts/lib/openssl-tls12.conf to work around
TLS failures with older OpenSSL in the builder image. Set
ZF_BUILD_CONTAINER_OPENSSL_CONF=0 to disable, or ZF_BUILD_CONTAINER_OPENSSL_CONF
to override the conf path.
USAGE
}

preset=""
jobs="${ZF_BUILD_JOBS:-}"
run_test=0
shell_mode=0
run_as_root=0
build_args=0

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --shell)
      shell_mode=1
      ;;
    --root)
      run_as_root=1
      ;;
    --jobs)
      shift
      [[ "$#" -gt 0 ]] || zf_fail_arg "missing value for --jobs"
      jobs="$1"
      build_args=1
      ;;
    --jobs=*)
      jobs="${1#--jobs=}"
      [[ -n "$jobs" ]] || zf_fail_arg "missing value for --jobs"
      build_args=1
      ;;
    --test)
      run_test=1
      build_args=1
      ;;
    -h|--help|help)
      usage
      exit 0
      ;;
    --)
      shift
      [[ "$#" -le 1 ]] || zf_fail_arg "expected at most one preset"
      if [[ "$#" -eq 1 ]]; then
        [[ -z "$preset" ]] || zf_fail_arg "preset specified more than once"
        preset="$1"
        build_args=1
      fi
      break
      ;;
    -*)
      zf_fail_arg "unknown option: $1"
      ;;
    *)
      [[ -z "$preset" ]] || zf_fail_arg "preset specified more than once"
      preset="$1"
      build_args=1
      ;;
  esac
  shift
done

if [[ "$shell_mode" -eq 1 && "$build_args" -eq 1 ]]; then
  zf_fail_arg "--shell cannot be combined with build arguments"
fi

WORKDIR="$(zf_build_container_workdir)"
IMAGE="$(zf_build_container_image)"

zf_build_container_args "$WORKDIR" "$run_as_root"

if [[ "$shell_mode" -eq 1 ]]; then
  if [[ "$run_as_root" -eq 0 ]]; then
    zf_log "shell in container: $IMAGE"
    zf_log "workdir: $WORKDIR"
    zf_log "running as current user $(id -u):$(id -g) with HOME=$WORKDIR"
  else
    zf_log "shell in container: $IMAGE"
    zf_log "workdir: $WORKDIR"
    zf_log "running as root with HOME=$WORKDIR"
  fi

  exec docker run -it \
    --name zfleet-builder \
    "${ZF_BUILD_CONTAINER_ARGS[@]}" \
    "$IMAGE" \
    /bin/bash
fi

preset="${preset:-$(zf_default_preset)}"

remote_script=""
printf -v remote_script '%q ' ./scripts/build.sh "$preset"
if [[ -n "$jobs" ]]; then
  remote_script+="--jobs $(printf '%q' "$jobs") "
fi
remote_script="${remote_script%" "}"
if [[ "$run_test" -eq 1 ]]; then
  remote_script+=" && ./scripts/test.sh $(printf '%q' "$preset")"
fi

zf_log "building in container: $IMAGE"
zf_log "workdir: $WORKDIR"
zf_log "preset: $preset"

docker run \
  "${ZF_BUILD_CONTAINER_ARGS[@]}" \
  "$IMAGE" \
  /bin/bash -lc "$remote_script"
