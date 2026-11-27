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

# One "release tag already exists" covered three unrelated states, so the
# classifier has to keep them apart. A lookup that could not answer is its own
# answer: every recovery the publisher prints from here deletes something.
expect_equal "$(classify_release_tag_collision absent absent absent)" "none" \
  "nothing exists anywhere"
expect_equal "$(classify_release_tag_collision published present absent)" "published" \
  "a published release"
expect_equal "$(classify_release_tag_collision draft absent absent)" "draft" \
  "a draft holding the tag name before the tag exists"
expect_equal "$(classify_release_tag_collision absent present absent)" "remote-tag" \
  "a tag on origin whose release creation failed"
expect_equal "$(classify_release_tag_collision absent absent present)" "local-tag" \
  "a tag left behind in this clone"
expect_equal "$(classify_release_tag_collision unknown absent absent)" "unknown-release" \
  "an unanswered release lookup is not an absent release"
expect_equal "$(classify_release_tag_collision absent unknown present)" "unknown-remote" \
  "an unanswered remote lookup is not an absent tag"

# Each message has to carry the command that clears the state it describes,
# and the local-tag case has to show both commits: that is what separates a
# leftover from a tag someone created on purpose.
local_report="$(report_release_tag_collision local-tag v1.2.3 owner/repo abc1234 def5678 "" 2>&1)"
[[ "${local_report}" == *"git tag -d v1.2.3"* ]] \
  || fail "the local-tag message does not say how to remove the tag"
[[ "${local_report}" == *abc1234* && "${local_report}" == *def5678* ]] \
  || fail "the local-tag message does not contrast the stray tag with HEAD"
remote_report="$(report_release_tag_collision remote-tag v1.2.3 owner/repo "" def5678 "" 2>&1)"
[[ "${remote_report}" == *"make delete-release RELEASE_TAG=v1.2.3"* ]] \
  || fail "the remote-tag message does not say how to drop the tag"
[[ "${remote_report}" == *"in progress"* ]] \
  || fail "the remote-tag message offers deletion without warning about a live publish"
# Deleting the tag alone does not unblock a retry; the staging directory stops
# the next run a few lines later.
staged_report="$(report_release_tag_collision local-tag v1.2.3 owner/repo abc1234 def5678 /tmp/staged 2>&1)"
[[ "${staged_report}" == *"/tmp/staged"* ]] \
  || fail "a leftover staging directory is not mentioned in the recovery advice"

# Publishing the tag is two steps, and anything that ends the process between
# them leaves a tag the next run cannot get past: the version comes from the
# remote, so a local-only tag is never counted but still collides.
tag_repo="$(mktemp -d "${TEST_TMPDIR:-/tmp}/publish-tag-XXXXXX")"
trap 'rm -rf "${tag_repo}"' EXIT
# Redirecting the config files is not enough on its own: an inherited
# GIT_CONFIG would send the fixture's config writes to the developer's own
# file, and GIT_CONFIG_COUNT can rewrite the fixture's remote URL.
unset GIT_CONFIG GIT_CONFIG_COUNT GIT_DIR GIT_WORK_TREE
export GIT_CONFIG_GLOBAL="${tag_repo}/gitconfig" GIT_CONFIG_SYSTEM=/dev/null
export HOME="${tag_repo}/home"
mkdir -p "${HOME}"
: > "${GIT_CONFIG_GLOBAL}"
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

# A tag that appeared between the final check and this call belongs to someone
# else. Pushing it would attach these packages to another commit, and the
# cleanup must not delete it either.
other_head="$(git -C "${work}" commit --quiet --allow-empty -m "another commit" \
  && git -C "${work}" rev-parse HEAD)"
git -C "${work}" tag -a v9.9.6 -m "someone else's tag" "${other_head}"
( cd "${work}" && push_release_tag v9.9.6 "${work_head}" "${tag_repo}/remote.git" ) \
  && fail "push_release_tag pushed a tag it did not create"
expect_equal "$(git -C "${work}" rev-parse "refs/tags/v9.9.6^{commit}")" "${other_head}" \
  "a tag this run did not create was left untouched"
if git -C "${tag_repo}/remote.git" rev-parse -q --verify refs/tags/v9.9.6 >/dev/null; then
  fail "a tag this run did not create was pushed to the remote"
fi

# Interrupted mid-push, which is how the tag survived before: the old code
# only cleaned up when git push returned a failing status. SIGTERM rather than
# SIGINT, because a shell started as a background job inherits SIGINT ignored
# and then cannot install the handler at all.
signal_status=0
(
  cd "${work}"
  git() {
    if [[ "${1:-}" == "push" ]]; then
      kill -TERM "${BASHPID}"
      sleep 10
    fi
    command git "$@"
  }
  push_release_tag v9.9.8 "${work_head}" "${tag_repo}/remote.git"
) || signal_status=$?
expect_equal "${signal_status}" "143" "an interrupted push exits on the signal"
expect_no_tag v9.9.8 "an interrupted push"

# The same interrupt after the remote accepted the push: the tag is on the
# remote now, so the cleanup has to leave the local copy alone.
accepted_status=0
(
  cd "${work}"
  git() {
    if [[ "${1:-}" == "push" ]]; then
      command git "$@"
      kill -TERM "${BASHPID}"
      sleep 10
      return 0
    fi
    command git "$@"
  }
  push_release_tag v9.9.5 "${work_head}" "${tag_repo}/remote.git"
) >/dev/null 2>&1 || accepted_status=$?
expect_equal "${accepted_status}" "143" "an interrupt after a successful push exits on the signal"
git -C "${work}" rev-parse -q --verify refs/tags/v9.9.5 >/dev/null \
  || fail "the cleanup deleted a tag the remote had already accepted"

# The ordinary success path keeps the tag on both sides.
( cd "${work}" && push_release_tag v9.9.7 "${work_head}" "${tag_repo}/remote.git" ) \
  || fail "push_release_tag failed against a reachable remote"
git -C "${work}" rev-parse -q --verify refs/tags/v9.9.7 >/dev/null \
  || fail "a pushed release tag was deleted from the local clone"
git -C "${tag_repo}/remote.git" rev-parse -q --verify refs/tags/v9.9.7 >/dev/null \
  || fail "the release tag never reached the remote"

# The state lookup the classifier runs on, against a real remote.
expect_equal "$(remote_tag_state "${tag_repo}/remote.git" v9.9.7)" "present" \
  "a tag the remote has"
expect_equal "$(remote_tag_state "${tag_repo}/remote.git" v0.0.0)" "absent" \
  "a tag the remote does not have"
expect_equal "$(remote_tag_state "${tag_repo}/missing.git" v0.0.0)" "unknown" \
  "a remote that cannot be reached is not an absent tag"

rm -rf "${tag_repo}"
trap - EXIT

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
