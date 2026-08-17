#!/bin/bash
# Build the complete HStream Debian matrix and publish it as the next semantic
# patch release. No tag or GitHub object is created until every package has
# been built and its metadata has been validated.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage: scripts/publish_release.sh [--dry-run]

Builds Ubuntu 24.04 amd64, Ubuntu 26.04 amd64, and Jetson Ubuntu 22.04
arm64 packages. The first release is v0.1.0; later releases increment the
patch component of the highest existing strict vMAJOR.MINOR.PATCH tag.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

for command_name in git gh make dpkg-deb sha256sum makensis rsvg-convert icotool file; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "ERROR: required command not found: ${command_name}" >&2
    exit 1
  fi
done
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: GitHub CLI is not authenticated. Run 'gh auth login' first." >&2
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

# Git permits a fetch URL plus one or more unrelated pushurl values. Release
# metadata and Git tags must never be sent to different repositories.
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

# Ask the remote directly. The local origin/HEAD symref can remain stale after
# a repository changes its default branch.
default_branch="$(git ls-remote --symref origin HEAD | sed -n 's#^ref: refs/heads/\([^[:space:]]*\)[[:space:]]*HEAD$#\1#p')"
if [[ -z "${default_branch}" ]]; then
  echo "ERROR: could not determine origin's default branch." >&2
  exit 1
fi
git fetch --quiet origin "${default_branch}"

latest_remote_release_tag() {
  git ls-remote --tags --refs origin 'refs/tags/v*' \
    | awk '$2 ~ /^refs\/tags\/v(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$/ {
        sub("^refs/tags/", "", $2); print $2
      }' \
    | sort -V \
    | tail -n 1
}

latest_tag="$(latest_remote_release_tag)"
if [[ -z "${latest_tag}" ]]; then
  release_tag="v0.1.0"
else
  version="${latest_tag#v}"
  IFS=. read -r major minor patch <<< "${version}"
  release_tag="v${major}.${minor}.$((10#${patch} + 1))"
fi
package_version="${release_tag#v}"

source_revision="$(git rev-parse HEAD)"
remote_revision="$(git rev-parse "refs/remotes/origin/${default_branch}")"
release_dir="${TOPDIR}/dist/releases/${release_tag}"

echo "Next release: ${release_tag}"
echo "Source commit: ${source_revision}"
echo "Artifacts:"
echo "  hstream_${release_tag}_ubuntu24.04_amd64.deb"
echo "  hstream_${release_tag}_ubuntu26.04_amd64.deb"
echo "  hstream_${release_tag}_jetson-ubuntu22.04_arm64.deb"
echo "  hugin_2022.0.0+dfsg.orig.tar.xz"
echo "  libvigraimpex_1.11.1+dfsg.orig.tar.xz"
echo "  hstream_${release_tag}_jetson-hugin-build.sh"
echo "  hstream_${release_tag}_windows-wsl-setup.exe"
echo "  SHA256SUMS"

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "Dry run only; no packages, tags, or GitHub releases were created."
  exit 0
fi

if ! git diff --quiet HEAD -- || ! git diff --cached --quiet; then
  echo "ERROR: refusing to publish with tracked or staged changes." >&2
  exit 1
fi
unexpected_untracked="$({
  git ls-files --others --exclude-standard \
    | grep -Ev '^(bazelisk|run|stitching-calibration-note[.]txt|dist/|dist-staging/|output_workdirs/|bazel-[^/]+(/|$))'
} || true)"
if [[ -n "${unexpected_untracked}" ]]; then
  echo "ERROR: refusing to publish with untracked source files:" >&2
  while IFS= read -r source_path; do
    printf '  %s\n' "${source_path}" >&2
  done <<< "${unexpected_untracked}"
  exit 1
fi
if [[ "${source_revision}" != "${remote_revision}" ]]; then
  echo "ERROR: HEAD must exactly match origin/${default_branch} before publishing." >&2
  echo "  HEAD:                    ${source_revision}" >&2
  echo "  origin/${default_branch}: ${remote_revision}" >&2
  exit 1
fi
if git show-ref --verify --quiet "refs/tags/${release_tag}" ||
   git ls-remote --exit-code --tags origin "refs/tags/${release_tag}" >/dev/null 2>&1 ||
   gh release view "${release_tag}" --repo "${repository}" >/dev/null 2>&1; then
  echo "ERROR: release tag already exists: ${release_tag}" >&2
  exit 1
fi
if [[ -e "${release_dir}" ]]; then
  echo "ERROR: release staging directory already exists: ${release_dir}" >&2
  echo "Remove or archive it explicitly before retrying." >&2
  exit 1
fi

build_dir="${release_dir}/build"
mkdir -p "${build_dir}/ubuntu24.04" "${build_dir}/ubuntu26.04" "${build_dir}/jetson"

make deb-ubuntu24 PACKAGE_VERSION="${release_tag}" DEB_OUTPUT_DIR="${build_dir}/ubuntu24.04"
make deb-ubuntu26 PACKAGE_VERSION="${release_tag}" DEB_OUTPUT_DIR="${build_dir}/ubuntu26.04"
make deb-jetson PACKAGE_VERSION="${release_tag}" JETSON_DEB_OUTPUT_DIR="${build_dir}/jetson"
make windows-installer \
  WINDOWS_INSTALLER_VERSION="${release_tag}" \
  WINDOWS_INSTALLER_OUTPUT_DIR="${build_dir}/windows"

validate_and_stage_deb() {
  local source_path="$1"
  local expected_arch="$2"
  local expected_ubuntu="$3"
  local expected_platform="$4"
  local published_name="$5"
  local actual_platform

  if [[ ! -f "${source_path}" ]]; then
    echo "ERROR: expected package was not created: ${source_path}" >&2
    exit 1
  fi
  if [[ "$(dpkg-deb -f "${source_path}" Package)" != "hstream" ||
        "$(dpkg-deb -f "${source_path}" Version)" != "${package_version}" ||
        "$(dpkg-deb -f "${source_path}" Architecture)" != "${expected_arch}" ||
        "$(dpkg-deb -f "${source_path}" X-HStream-Target-Ubuntu)" != "${expected_ubuntu}" ||
        "$(dpkg-deb -f "${source_path}" X-HStream-Source-Commit)" != "${source_revision}" ]]; then
    echo "ERROR: package metadata does not match release ${release_tag}: ${source_path}" >&2
    dpkg-deb --info "${source_path}" >&2
    exit 1
  fi
  actual_platform="$(dpkg-deb -f "${source_path}" X-HStream-Target-Platform 2>/dev/null || true)"
  if [[ "${actual_platform}" != "${expected_platform}" ]]; then
    echo "ERROR: package platform is ${actual_platform:-missing}, expected ${expected_platform}: ${source_path}" >&2
    exit 1
  fi
  install -m 0644 "${source_path}" "${release_dir}/${published_name}"
}

validate_and_stage_deb \
  "${build_dir}/ubuntu24.04/hstream_${package_version}_amd64.deb" \
  amd64 24.04 desktop "hstream_${release_tag}_ubuntu24.04_amd64.deb"
validate_and_stage_deb \
  "${build_dir}/ubuntu26.04/hstream_${package_version}_amd64.deb" \
  amd64 26.04 desktop "hstream_${release_tag}_ubuntu26.04_amd64.deb"
validate_and_stage_deb \
  "${build_dir}/jetson/hstream_${package_version}_arm64.deb" \
  arm64 22.04 jetson "hstream_${release_tag}_jetson-ubuntu22.04_arm64.deb"
windows_installer="${build_dir}/windows/hstream_${release_tag}_windows-wsl-setup.exe"
if [[ ! -f "${windows_installer}" ]] ||
   [[ "$(file -Lb "${windows_installer}")" != *"PE32 executable"* ]]; then
  echo "ERROR: expected Windows WSL bootstrapper was not created: ${windows_installer}" >&2
  exit 1
fi
install -m 0755 "${windows_installer}" \
  "${release_dir}/hstream_${release_tag}_windows-wsl-setup.exe"

# The Jetson package redistributes GPLv2 Hugin binaries. Accompany them with
# the exact verified corresponding-source archive and the build-control script
# used by this tag. VIGRA's verified source is included as well so the complete
# native calibration toolchain can be reconstructed from release assets.
hugin_build_script_asset="${release_dir}/hstream_${release_tag}_jetson-hugin-build.sh"
git show "${source_revision}:scripts/build_hugin_tools_jetson.sh" > "${hugin_build_script_asset}"
chmod 0755 "${hugin_build_script_asset}"
"${hugin_build_script_asset}" --export-sources="${release_dir}"

(
  cd "${release_dir}"
  sha256sum ./*.deb ./*.exe ./*.tar.xz ./*.sh > SHA256SUMS
)

# Recheck after the long builds so a concurrent publisher cannot claim the
# same version while this process was compiling packages.
git fetch --quiet origin "${default_branch}"
new_latest_tag="$(latest_remote_release_tag)"
if [[ "${new_latest_tag}" != "${latest_tag}" ]] ||
   [[ "$(git rev-parse HEAD)" != "${source_revision}" ]] ||
   [[ "$(git rev-parse "refs/remotes/origin/${default_branch}")" != "${remote_revision}" ]] ||
   git show-ref --verify --quiet "refs/tags/${release_tag}" ||
   gh release view "${release_tag}" --repo "${repository}" >/dev/null 2>&1; then
  echo "ERROR: release tags or source revisions changed while packages were building;" >&2
  echo "artifacts remain in ${release_dir}." >&2
  exit 1
fi

git tag -a "${release_tag}" "${source_revision}" -m "HStream ${release_tag}"
if ! git push origin "refs/tags/${release_tag}"; then
  git tag -d "${release_tag}" >/dev/null
  echo "ERROR: could not push release tag ${release_tag}." >&2
  exit 1
fi

release_assets=(
  "${release_dir}"/*.deb
  "${release_dir}"/*.exe
  "${release_dir}"/*.tar.xz
  "${release_dir}"/*.sh
  "${release_dir}/SHA256SUMS"
)
if ! gh release create "${release_tag}" "${release_assets[@]}" \
    --repo "${repository}" --verify-tag --generate-notes --title "HStream ${release_tag}"; then
  echo "ERROR: tag ${release_tag} was pushed, but GitHub release publication failed." >&2
  echo "After inspecting the failure, retry publication or run:" >&2
  echo "  make delete-release RELEASE_TAG=${release_tag}" >&2
  exit 1
fi

echo "Published HStream ${release_tag}:"
if ! gh release view "${release_tag}" --repo "${repository}" --json url --jq '.url'; then
  echo "WARNING: release publication succeeded, but its URL could not be queried." >&2
fi
