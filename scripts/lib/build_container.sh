#!/usr/bin/env bash

zf_build_container_workdir() {
  printf '%s\n' "${ZF_BUILD_CONTAINER_WORKDIR:-$(pwd)}"
}

zf_build_container_image() {
  if [[ -n "${ZF_BUILDER_IMAGE:-}" ]]; then
    printf '%s\n' "$ZF_BUILDER_IMAGE"
    return 0
  fi

  local version_file="$ZF_REPO_ROOT/ci/images/version.txt"
  local image_tag
  [[ -f "$version_file" ]] ||
    zf_fail_exec "missing $version_file; set ZF_BUILDER_IMAGE to override"
  image_tag="$(tr -d '[:space:]' < "$version_file")"
  [[ -n "$image_tag" ]] ||
    zf_fail_exec "empty image tag in $version_file; set ZF_BUILDER_IMAGE to override"

  printf 'ghcr.io/fjzhangzzzzzz/zfleet-builder:%s\n' "$image_tag"
}

zf_build_container_proxy_var_names() {
  printf '%s\n' \
    HTTP_PROXY HTTPS_PROXY http_proxy https_proxy \
    ALL_PROXY all_proxy \
    NO_PROXY no_proxy \
    FTP_PROXY ftp_proxy \
    GIT_HTTP_PROXY GIT_HTTPS_PROXY
}

zf_build_container_proxy_enabled() {
  local var

  while IFS= read -r var; do
    [[ -n "${!var:-}" ]] && return 0
  done < <(zf_build_container_proxy_var_names)

  return 1
}

# Appends -e VAR=value for each non-empty proxy-related env var.
zf_build_container_append_proxy_args() {
  local var
  local forwarded=()

  while IFS= read -r var; do
    [[ -n "${!var:-}" ]] || continue
    ZF_BUILD_CONTAINER_ARGS+=(-e "$var=${!var}")
    forwarded+=("$var")
  done < <(zf_build_container_proxy_var_names)

  if [[ ${#forwarded[@]} -gt 0 ]]; then
    zf_log "forwarding proxy env: ${forwarded[*]}"
  fi
}

zf_build_container_default_openssl_conf() {
  printf '%s/scripts/lib/openssl-tls12.conf\n' "$ZF_REPO_ROOT"
}

# Appends -e OPENSSL_CONF=... when proxy is detected, unless disabled.
# Args: workdir
zf_build_container_append_openssl_conf_args() {
  local workdir="$1"
  local conf_path
  local openssl_conf_in_container

  if [[ "${ZF_BUILD_CONTAINER_OPENSSL_CONF:-}" == "0" ]]; then
    return 0
  fi

  if [[ -n "${ZF_BUILD_CONTAINER_OPENSSL_CONF:-}" ]]; then
    conf_path="$ZF_BUILD_CONTAINER_OPENSSL_CONF"
    openssl_conf_in_container="$conf_path"
  elif zf_build_container_proxy_enabled; then
    conf_path="$(zf_build_container_default_openssl_conf)"
    openssl_conf_in_container="$workdir/scripts/lib/openssl-tls12.conf"
  else
    return 0
  fi

  [[ -f "$conf_path" ]] ||
    zf_fail_exec "OpenSSL conf not found: $conf_path"

  ZF_BUILD_CONTAINER_ARGS+=(-e "OPENSSL_CONF=$openssl_conf_in_container")
  zf_log "applying OpenSSL TLS 1.2 conf: $openssl_conf_in_container"
}

# Populates ZF_BUILD_CONTAINER_ARGS for docker run.
# Args: workdir run_as_root(0|1)
zf_build_container_args() {
  local workdir="$1"
  local run_as_root="${2:-0}"

  ZF_BUILD_CONTAINER_ARGS=(
    --rm
    --network host
    -w "$workdir"
    -v "$workdir:$workdir"
    -e "HOME=$workdir"
    -e "XDG_CACHE_HOME=$workdir/.cache"
  )

  if [[ "$run_as_root" -eq 0 ]]; then
    ZF_BUILD_CONTAINER_ARGS+=(-u "$(id -u):$(id -g)")
  fi

  if [[ -d "${HOME}/.ssh" ]]; then
    ZF_BUILD_CONTAINER_ARGS+=(-v "${HOME}/.ssh:${workdir}/.ssh:ro")
  fi

  zf_build_container_append_proxy_args
  zf_build_container_append_openssl_conf_args "$workdir"
}
