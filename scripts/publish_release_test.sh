#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/publish_release.sh
source "${SCRIPT_DIR}/publish_release.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

expect_equal() {
  local actual="$1"
  local expected="$2"
  local label="$3"
  [[ "${actual}" == "${expected}" ]] || fail "${label}: expected '${expected}', got '${actual}'"
}

expect_platforms() {
  local excluded="$1"
  local expected="$2"
  local label="$3"
  local actual
  actual="$(resolve_release_platforms "${excluded}" | tr '\n' ' ')" \
    || fail "${label}: resolution failed for '${excluded}'"
  actual="${actual% }"
  [[ "${actual}" == "${expected}" ]] \
    || fail "${label}: expected '${expected}', got '${actual}'"
}

expect_rejected() {
  local excluded="$1"
  local label="$2"
  if resolve_release_platforms "${excluded}" >/dev/null 2>&1; then
    fail "${label}: '${excluded}' was accepted"
  fi
}

expect_platforms "" "ubuntu24 ubuntu26 jetson windows" "no exclusions publishes everything"
expect_platforms "jetson" "ubuntu24 ubuntu26 windows" "the broken-Jetson case"
expect_platforms "jetson,windows" "ubuntu24 ubuntu26" "comma-separated list"
expect_platforms "jetson windows" "ubuntu24 ubuntu26" "space-separated list"
expect_platforms " jetson , windows " "ubuntu24 ubuntu26" "surrounding whitespace"
expect_platforms "jetson,jetson" "ubuntu24 ubuntu26 windows" "repeated platform"
expect_platforms "ubuntu24.04,windows" "ubuntu26 jetson" "dotted Ubuntu spelling"
expect_platforms "ubuntu26.04" "ubuntu24 jetson windows" "dotted Ubuntu 26 spelling"

# A plain `read` stops at the first newline, which would publish a platform the
# caller asked to leave out instead of failing.
expect_platforms $'jetson\nwindows' "ubuntu24 ubuntu26" "newline-separated list"
expect_platforms $'\njetson' "ubuntu24 ubuntu26 windows" "leading newline"
expect_rejected $'\nubuntu24\nubuntu26\njetson\nwindows' "excluding everything across newlines"

expect_rejected "arm64" "unknown platform"
expect_rejected "jetson,sbsa" "unknown platform beside a known one"
expect_rejected "Jetson" "platform names are case-sensitive"
expect_rejected "ubuntu24,ubuntu26,jetson,windows" "excluding everything"

# hstream-wsl.ps1 downloads hstream_<tag>_ubuntu24.04_amd64.deb from the same
# release, so that package cannot be dropped while the installer ships.
expect_rejected "ubuntu24" "Windows installer without its Ubuntu 24.04 payload"
expect_platforms "ubuntu24,windows" "ubuntu26 jetson" "Windows dropped along with its payload"
installer_deb="$(sed -n 's/.*\$hstreamName = "hstream_\${VersionTag}_\(.*\)"$/\1/p' \
  "${SCRIPT_DIR}/../packaging/windows/hstream-wsl.ps1")"
[[ "${installer_deb}" == "ubuntu24.04_amd64.deb" ]] \
  || fail "the Windows installer now downloads '${installer_deb}'; update the ubuntu24 dependency"

for platform in "${RELEASE_PLATFORMS[@]}"; do
  platform_label "${platform}" >/dev/null \
    || fail "no release-notes label for platform '${platform}'"
done

# Publishing the tag is two steps, and anything that ends the process between
# them leaves a tag the next run cannot get past: the version comes from the
# remote, so a local-only tag is never counted but still collides.
tag_repo="$(mktemp -d "${TEST_TMPDIR:-/tmp}/publish-tag-XXXXXX")"
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
work="${tag_repo}/work"
git init --quiet --bare "${tag_repo}/remote.git"
git init --quiet "${work}"
git -C "${work}" config user.email "release-test@example.com"
git -C "${work}" config user.name "Release Test"
git -C "${work}" commit --quiet --allow-empty -m "base commit"
work_head="$(git -C "${work}" rev-parse HEAD)"

expect_no_tag() {
  local tag="$1" label="$2"
  if git -C "${work}" rev-parse -q --verify "refs/tags/${tag}" >/dev/null; then
    fail "${label}: ${tag} was left behind in the local clone"
  fi
}

# An unreachable remote: the push fails on its own.
( cd "${work}" && push_release_tag v9.9.9 "${work_head}" "${tag_repo}/missing.git" ) \
  && fail "push_release_tag reported success against an unreachable remote"
expect_no_tag v9.9.9 "a failed push"

# Interrupted mid-push, which is how the tag survived before: the old code
# only cleaned up when git push returned a failing status.
interrupt_status=0
(
  cd "${work}"
  git() {
    if [[ "${1:-}" == "push" ]]; then
      kill -INT "${BASHPID}"
      sleep 10
    fi
    command git "$@"
  }
  push_release_tag v9.9.8 "${work_head}" "${tag_repo}/remote.git"
) || interrupt_status=$?
expect_equal "${interrupt_status}" "130" "an interrupted push exits on the signal"
expect_no_tag v9.9.8 "an interrupted push"

# The cleanup must not reach a tag that already made it to the remote; that
# one has to survive for the release to be finished or deleted deliberately.
( cd "${work}" && push_release_tag v9.9.7 "${work_head}" "${tag_repo}/remote.git" ) \
  || fail "push_release_tag failed against a reachable remote"
git -C "${work}" rev-parse -q --verify refs/tags/v9.9.7 >/dev/null \
  || fail "a pushed release tag was deleted from the local clone"
git -C "${tag_repo}/remote.git" rev-parse -q --verify refs/tags/v9.9.7 >/dev/null \
  || fail "the release tag never reached the remote"
rm -rf "${tag_repo}"

# Stage a Jetson-excluded release the way the publisher does and check that
# what it hashes is exactly what it uploads, with no stray glob. The staging
# directory carries an ampersand on purpose: "${staged_assets[@]/#/${dir}/}"
# would drop it under bash 5.2's patsub_replacement and name files that do not
# exist, and only after the release tag had already been pushed.
staging="$(mktemp -d "${TEST_TMPDIR:-/tmp}/publish-r&d-XXXXXX")"
trap 'rm -rf "${staging}"' EXIT
[[ "${staging}" == *"r&d"* ]] || fail "the staging path lost its ampersand before the test ran"
staged_assets=(
  "hstream_v1.2.3_ubuntu24.04_amd64.deb"
  "hstream_v1.2.3_ubuntu26.04_amd64.deb"
  "hstream_v1.2.3_windows-wsl-setup.exe"
  "install-hstream-deb.sh"
)
for asset in "${staged_assets[@]}"; do
  printf 'contents of %s\n' "${asset}" > "${staging}/${asset}"
done

# These two loops are the publisher's, verbatim.
checksum_names=()
for asset in "${staged_assets[@]}"; do
  checksum_names+=("./${asset}")
done
release_assets=()
for asset in "${staged_assets[@]}"; do
  release_assets+=("${staging}/${asset}")
done
release_assets+=("${staging}/SHA256SUMS")

(cd "${staging}" && sha256sum "${checksum_names[@]}" > SHA256SUMS)
(cd "${staging}" && sha256sum --check --status SHA256SUMS) \
  || fail "the staged checksum file does not verify"
expect_equal "$(wc -l < "${staging}/SHA256SUMS")" "${#staged_assets[@]}" \
  "every staged asset is hashed exactly once"
if grep -q '[*]' "${staging}/SHA256SUMS"; then
  fail "an unexpanded glob reached SHA256SUMS"
fi
for asset in "${release_assets[@]}"; do
  [[ -f "${asset}" ]] || fail "upload list names a file that does not exist: ${asset}"
done
expect_equal "${#release_assets[@]}" "$((${#staged_assets[@]} + 1))" \
  "the upload list is the staged assets plus SHA256SUMS"

# Every name in SHA256SUMS must be uploaded, so a consumer can verify what it
# downloaded against the entry that describes it.
while read -r _ checksum_name; do
  [[ -f "${staging}/${checksum_name#./}" ]] \
    || fail "SHA256SUMS names a file that is not staged: ${checksum_name}"
done < "${staging}/SHA256SUMS"

echo "PASS"
