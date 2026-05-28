#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

usage() {
  cat >&2 <<'EOF'
Usage: ./scripts/generate-release-notes.sh <component> <tag>
EOF
}

[[ $# -eq 2 ]] || zf_fail_arg "expected <component> <tag>"

component="$1"
tag="$2"

zf_validate_component "$component" ||
  zf_fail_arg "invalid component: $component; expected $(zf_known_components_description)"

case "$tag" in
  "$component"/v*)
    ;;
  *)
    zf_fail_arg "tag must match <component>/v<semver>, got: $tag"
    ;;
esac

repo_root="$ZF_REPO_ROOT"
current_version="${tag#${component}/v}"
[[ -n "$current_version" ]] || zf_fail_exec "empty version parsed from tag: $tag"
zf_is_safe_segment "$current_version" ||
  zf_fail_exec "invalid version parsed from tag: $current_version"

previous_tag="$(
  git -C "$repo_root" tag --list "${component}/v*" --sort=-version:refname |
    awk -v current="$tag" '$0 != current { print; exit }'
)"

if [[ -n "$previous_tag" ]]; then
  range="$previous_tag..$tag"
  range_heading="上一个同组件 tag：\`$previous_tag\`"
else
  range="$tag"
  range_heading="当前 tag 为首个同组件发布 tag"
fi

component_dir="$repo_root/apps/$component"

{
  printf '# %s %s\n\n' "$component" "$current_version"
  printf '## 范围\n\n'
  printf '- %s\n' "$range_heading"
  printf '- 提交范围：`%s`\n\n' "$range"

  printf '## 变更摘要\n\n'
  commits="$(git -C "$repo_root" log --no-merges --format='%s' "$range" -- "$component_dir" 2>/dev/null || true)"
  if [[ -n "$commits" ]]; then
    printf '%s\n' "$commits" | sed 's/^/- /'
  else
    printf -- '- 无可识别的组件内变更提交，或提交信息未按规范书写。\n'
  fi
  printf '\n'

  printf '## 兼容性\n\n'
  printf '- 该草稿由 CI 自动生成，发布前需人工确认版本号、制品和变更范围。\n'
  printf '- 若包含破坏性变更，应在此补充升级或回滚注意事项。\n'
} 
