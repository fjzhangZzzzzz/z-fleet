#!/usr/bin/env bash
set -euo pipefail
server_url=
control_url=
token=
channel=stable
build_type=release
root=/opt/zfleet
while [[ $# -gt 0 ]]; do
  case "$1" in
    --server-url) server_url="$2"; shift 2 ;;
    --control-url) control_url="$2"; shift 2 ;;
    --token) token="$2"; shift 2 ;;
    --channel) channel="$2"; shift 2 ;;
    --build-type) build_type="$2"; shift 2 ;;
    --root) root="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$server_url" && -n "$control_url" && -n "$token" ]] ||
  { echo "--server-url, --control-url and --token are required" >&2; exit 2; }
case "$(uname -m)" in
  x86_64|amd64) arch="x86_64" ;;
  aarch64|arm64) arch="arm64" ;;
  *) echo "unsupported architecture" >&2; exit 1 ;;
esac
http_get() {
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$1"
    return
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -qO- "$1"
    return
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$1" <<'PY'
import sys
from urllib.request import urlopen
with urlopen(sys.argv[1]) as response:
    sys.stdout.buffer.write(response.read())
PY
    return
  fi
  echo "no download tool found (curl/wget/python3)" >&2
  exit 1
}
download_to() {
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$1" -o "$2"
    return
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -qO "$2" "$1"
    return
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$1" "$2" <<'PY'
import sys
from urllib.request import urlopen
with urlopen(sys.argv[1]) as response, open(sys.argv[2], "wb") as output:
    output.write(response.read())
PY
    return
  fi
  echo "no download tool found (curl/wget/python3)" >&2
  exit 1
}
options="$(http_get "$server_url/api/v1/install/options?platform=linux&arch=$arch&channel=$channel&build_type=$build_type")"
json_field() { python3 -c 'import json,sys; value=json.loads(sys.argv[1]); cur=value
for name in sys.argv[2].split("."): cur=cur[name]
print(cur)' "$options" "$1"; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
installer_url="$(json_field installer.download_url)"
installer_sha="$(json_field installer.sha256)"
agent_url="$(json_field agent.download_url)"
agent_sha="$(json_field agent.sha256)"
download_to "$server_url$installer_url" "$tmp/installer.zip"
download_to "$server_url$agent_url" "$tmp/agent.zip"
echo "$installer_sha  $tmp/installer.zip" | sha256sum -c -
echo "$agent_sha  $tmp/agent.zip" | sha256sum -c -
unzip -q "$tmp/installer.zip" -d "$tmp/installer"
chmod +x "$tmp/installer/payload/bin/zfleet_installer"
"$tmp/installer/payload/bin/zfleet_installer" apply --root "$root" --package "$tmp/installer.zip"
installer="$root/installer/bin/zfleet_installer"
"$installer" apply --root "$root" --package "$tmp/agent.zip"
agent="$root/agent/bin/zfleet_agent"
"$agent" --control-url "$control_url" --registration-token "$token" >/dev/null 2>&1 &
