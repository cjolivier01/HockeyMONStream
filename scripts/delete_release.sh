#!/bin/bash
# Delete one explicit semantic-version GitHub release, all of its uploaded
# assets, and both the remote and local tag.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
release_tag="${1:-}"

if [[ ! "${release_tag}" =~ ^v(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$ ]]; then
  echo "ERROR: an exact semantic release tag is required." >&2
  echo "Usage: make delete-release RELEASE_TAG=v0.1.0" >&2
  exit 2
fi
if ! command -v gh >/dev/null 2>&1 || ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: authenticated GitHub CLI is required." >&2
  exit 1
fi

cd "${TOPDIR}"
normalize_github_repository() {
  local remote_url="$1"
  local remote_repository
  case "${remote_url}" in
    git@github.com:*) remote_repository="${remote_url#git@github.com:}" ;;
    ssh://git@github.com/*) remote_repository="${remote_url#ssh://git@github.com/}" ;;
    https://github.com/*) remote_repository="${remote_url#https://github.com/}" ;;
    *) return 1 ;;
  esac
  remote_repository="${remote_repository%.git}"
  [[ "${remote_repository}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || return 1
  printf '%s\n' "${remote_repository}"
}

fetch_urls="$(git remote get-url --all origin)" || {
  echo "ERROR: could not resolve origin fetch URLs." >&2
  exit 1
}
push_urls="$(git remote get-url --push --all origin)" || {
  echo "ERROR: could not resolve origin push URLs." >&2
  exit 1
}
declare -A origin_repositories=()
while IFS= read -r origin_url; do
  [[ -n "${origin_url}" ]] || continue
  repository_candidate="$(normalize_github_repository "${origin_url}")" || {
    echo "ERROR: origin has an unsupported GitHub fetch/push URL: ${origin_url}" >&2
    exit 1
  }
  origin_repositories["${repository_candidate}"]=1
done <<< "${fetch_urls}"$'\n'"${push_urls}"
if [[ "${#origin_repositories[@]}" -ne 1 ]]; then
  echo "ERROR: every origin fetch/push URL must resolve to the same GitHub repository." >&2
  printf '  %s\n' "${!origin_repositories[@]}" >&2
  exit 1
fi
repository="${!origin_repositories[*]}"

release_exists=0
remote_tag_exists=0
set +e
release_response="$(gh api --include --silent "repos/${repository}/releases/tags/${release_tag}" 2>&1)"
release_api_exit=$?
set -e
release_http_status="$(sed -n '1s#^HTTP/[0-9.]* \([0-9][0-9][0-9]\).*$#\1#p' <<< "${release_response}")"
case "${release_http_status}" in
  200)
    if [[ "${release_api_exit}" -ne 0 ]]; then
      echo "ERROR: GitHub returned HTTP 200 but gh failed while inspecting ${release_tag}." >&2
      exit 1
    fi
    release_exists=1
    ;;
  404) release_exists=0 ;;
  *)
    echo "ERROR: could not determine whether GitHub release ${release_tag} exists." >&2
    printf '%s\n' "${release_response}" >&2
    exit 1
    ;;
esac
if git ls-remote --exit-code --tags origin "refs/tags/${release_tag}" >/dev/null 2>&1; then
  remote_tag_exists=1
fi
if [[ "${release_exists}" -eq 0 && "${remote_tag_exists}" -eq 0 ]]; then
  echo "ERROR: neither a GitHub release nor a remote tag exists for ${release_tag}." >&2
  exit 1
fi

if [[ "${DELETE_RELEASE_CONFIRM:-0}" != "1" ]]; then
  if [[ ! -t 0 ]]; then
    echo "ERROR: interactive confirmation is required." >&2
    echo "Set DELETE_RELEASE_CONFIRM=1 only when intentionally deleting ${release_tag}." >&2
    exit 1
  fi
  printf 'Type %s to delete its GitHub release, assets, and tag: ' "${release_tag}" >&2
  IFS= read -r confirmation
  if [[ "${confirmation}" != "${release_tag}" ]]; then
    echo "Deletion cancelled." >&2
    exit 1
  fi
fi

if [[ "${release_exists}" -eq 1 ]]; then
  gh release delete "${release_tag}" --repo "${repository}" --cleanup-tag --yes
elif [[ "${remote_tag_exists}" -eq 1 ]]; then
  # A failed release upload can leave the newly pushed tag without a release.
  # Keep the recovery target useful for that exact partial-publication state.
  git push origin --delete "refs/tags/${release_tag}"
fi
if git show-ref --verify --quiet "refs/tags/${release_tag}"; then
  git tag -d "${release_tag}"
fi
echo "Deleted GitHub release (if present), uploaded assets, and tag: ${release_tag}"
