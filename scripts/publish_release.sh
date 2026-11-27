#!/bin/bash
# Build the complete HockeyMONStream package matrix and publish the next semantic
# patch release. No tag or GitHub object is created until every package has
# been built and its metadata has been validated.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRY_RUN=0

# 'make publish EXCLUDE=...' arrives here, out of reach of shell evaluation.
EXCLUDE_SPEC="${HSTREAM_RELEASE_EXCLUDE:-}"

usage() {
  cat <<'USAGE'
Usage: scripts/publish_release.sh [--dry-run] [--exclude=PLATFORMS]

Builds Ubuntu 24.04 amd64, Ubuntu 26.04 amd64, Jetson Ubuntu 24.04 arm64, and
Windows WSL packages. The first release is v0.1.0; later releases increment the
patch component of the highest strict vMAJOR.MINOR.PATCH tag on origin. A tag
that exists only in the local clone does not advance that number; if it
happens to carry the next version's name it blocks publication until it is
removed.

--exclude takes a comma- or space-separated list of platforms to leave out of
the release, for building a release when one target's hardware or toolchain is
unavailable. Supported names are ubuntu24, ubuntu26, jetson, and windows; the
option can be repeated. Excluding ubuntu24 also requires excluding windows,
because the Windows installer downloads the Ubuntu 24.04 package by name.
USAGE
}

# Build order, which is also the order artifacts are listed and staged.
RELEASE_PLATFORMS=(ubuntu24 ubuntu26 jetson windows)

platform_label() {
  case "$1" in
    ubuntu24) printf 'Ubuntu 24.04 amd64\n' ;;
    ubuntu26) printf 'Ubuntu 26.04 amd64\n' ;;
    jetson) printf 'NVIDIA Jetson Ubuntu 24.04 arm64\n' ;;
    windows) printf 'Windows 11 WSL installer\n' ;;
    *) return 1 ;;
  esac
}

canonical_platform() {
  case "$1" in
    ubuntu24|ubuntu24.04) printf 'ubuntu24\n' ;;
    ubuntu26|ubuntu26.04) printf 'ubuntu26\n' ;;
    jetson) printf 'jetson\n' ;;
    windows) printf 'windows\n' ;;
    *) return 1 ;;
  esac
}

# Prints the platforms to publish, one per line, in RELEASE_PLATFORMS order.
resolve_release_platforms() {
  local excluded_spec="$1"
  local -a tokens=() included=()
  local -A excluded=()
  local token canonical platform
  # -d '' consumes the whole value: a plain read would stop at the first
  # newline and silently ignore every exclusion after it.
  IFS=$', \n\t' read -r -d '' -a tokens <<< "${excluded_spec}" || true
  for token in ${tokens[@]+"${tokens[@]}"}; do
    [[ -n "${token}" ]] || continue
    if ! canonical="$(canonical_platform "${token}")"; then
      echo "ERROR: unknown release platform: ${token}" >&2
      echo "Supported platforms: ${RELEASE_PLATFORMS[*]}" >&2
      return 1
    fi
    excluded["${canonical}"]=1
  done
  for platform in "${RELEASE_PLATFORMS[@]}"; do
    [[ -n "${excluded[${platform}]:-}" ]] || included+=("${platform}")
  done
  if [[ "${#included[@]}" -eq 0 ]]; then
    echo "ERROR: every platform was excluded, so the release would contain no packages." >&2
    return 1
  fi
  # The Windows bootstrapper downloads hstream_<tag>_ubuntu24.04_amd64.deb from
  # its own release by name, so publishing it without that package ships an
  # installer that cannot complete.
  if [[ -n "${excluded[ubuntu24]:-}" && -z "${excluded[windows]:-}" ]]; then
    echo "ERROR: the Windows installer downloads this release's Ubuntu 24.04 amd64 package," >&2
    echo "so excluding ubuntu24 also requires excluding windows." >&2
    return 1
  fi
  printf '%s\n' "${included[@]}"
}

# Does the remote carry this tag? 'unknown' when the lookup itself failed:
# every caller below either deletes something or publishes on the answer, so a
# transport error must never read as an absent ref. git ls-remote --exit-code
# reserves status 2 for "no matching ref" and uses other statuses for errors.
remote_tag_state() {
  local remote="$1" tag="$2" status=0
  git ls-remote --exit-code --tags "${remote}" "refs/tags/${tag}" >/dev/null 2>&1 || status=$?
  case "${status}" in
    0) printf 'present\n' ;;
    2) printf 'absent\n' ;;
    *) printf 'unknown\n' ;;
  esac
}

# published | draft | absent | unknown. A draft is visible to its publisher
# under a tag that may not exist yet, so it cannot be reported as published.
github_release_state() {
  local repository="$1" tag="$2" stdout="" stderr="" status=0 errors
  errors="$(mktemp)" || { printf 'unknown\n'; return 0; }
  # Separate streams: gh writes diagnostics to stderr on success too, and
  # merging them would make "true" plus a warning read as a published release.
  stdout="$(gh release view "${tag}" --repo "${repository}" --json isDraft --jq '.isDraft' \
    2>"${errors}")" || status=$?
  stderr="$(cat "${errors}")"
  rm -f "${errors}"
  if [[ "${status}" -eq 0 ]]; then
    case "${stdout}" in
      true) printf 'draft\n' ;;
      false) printf 'published\n' ;;
      # A successful call that did not answer the question is not an answer.
      *) printf 'unknown\n' ;;
    esac
    return 0
  fi
  if [[ "${stderr}" == *"release not found"* ]]; then
    printf 'absent\n'
  else
    printf 'unknown\n'
  fi
}

# Set while a release tag exists locally but has not reached the remote yet.
unpushed_release_tag=""
unpushed_release_tag_object=""
unpushed_release_remote=""

# Remove the tag this run created, once it is clear the remote does not have
# it. Ownership is the recorded object id and nothing else: a tag whose id was
# never recorded belongs to whoever won the race to create the name, and
# deleting someone else's release tag is far worse than leaving one of ours
# behind for the next run to report.
#
# Pass 'push-failed' when git push reported a failure. That only changes the
# unreachable-remote case: the push almost certainly never landed, so clean up
# rather than strand a tag. After a signal, an unanswered remote means keep.
remove_unpushed_release_tag() {
  local tag="${unpushed_release_tag}" object="${unpushed_release_tag_object}"
  local push_failed="${1:-}" current="" remote_state
  [[ -n "${tag}" ]] || return 0
  if [[ -z "${object}" ]]; then
    # Created but not yet recorded, or never created at all. Only the first is
    # a leak, and it cannot be told apart from losing the creation race.
    current="$(git rev-parse -q --verify "refs/tags/${tag}" 2>/dev/null || true)"
    if [[ -n "${current}" ]]; then
      echo "WARNING: ${tag} exists but was not created by this run, or its id was never" >&2
      echo "recorded. Leaving it alone; check it before publishing again." >&2
    fi
    unpushed_release_tag=""
    return 0
  fi
  remote_state="$(remote_tag_state "${unpushed_release_remote}" "${tag}")"
  if [[ "${remote_state}" == "present" ]] \
    || [[ "${remote_state}" == "unknown" && "${push_failed}" != "push-failed" ]]; then
    echo "WARNING: leaving local tag ${tag} in place: ${unpushed_release_remote} may already have it." >&2
    unpushed_release_tag=""
    unpushed_release_tag_object=""
    return 0
  fi
  # update-ref checks the expected id under the ref lock, so a tag replaced
  # between here and the previous lookup is not deleted by name.
  if ! git update-ref -d "refs/tags/${tag}" "${object}" 2>/dev/null; then
    # Most likely the ref no longer points at this run's object, in which case
    # it belongs to someone else and must stay. Repeat the same id-checked
    # command rather than 'git tag -d', which would delete it by name.
    echo "WARNING: did not remove the unpushed tag ${tag}: it is gone already, or it no" >&2
    echo "longer points at the object this run created. Leave it alone unless this" >&2
    echo "command, which refuses any other object, succeeds:" >&2
    echo "  git update-ref -d refs/tags/${tag} ${object}" >&2
  fi
  # Cleared last: a signal arriving during the work above re-enters this
  # function with the claim intact rather than losing track of the tag.
  unpushed_release_tag=""
  unpushed_release_tag_object=""
}

# Create the release tag and push it, removing the local tag again if anything
# between the two ends this process. An unpushed tag left behind blocks every
# later publish without ever advancing the version: the next number comes from
# the remote, so a local-only tag is never counted and always collides.
# Signals need handlers of their own because a bash INT handler returns to the
# interrupted command instead of ending the script.
push_release_tag() {
  local release_tag="$1" source_revision="$2" remote="$3"
  local tag_object="" tagged_commit="" expected_commit=""
  # The caller runs this behind 'if !', which suppresses errexit for the whole
  # function, so every step has to be checked by hand. A name that appeared
  # since the last check belongs to someone else: pushing it would publish
  # another commit under this run's packages.
  if git rev-parse -q --verify "refs/tags/${release_tag}" >/dev/null 2>&1; then
    echo "ERROR: ${release_tag} appeared in this clone after the final check." >&2
    echo "Refusing to push a tag this run did not create." >&2
    return 1
  fi
  unpushed_release_remote="${remote}"
  trap remove_unpushed_release_tag EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  # Claimed before the tag exists, so a signal in the gap between these two
  # still reaches the cleanup; the name was just verified free.
  unpushed_release_tag="${release_tag}"
  unpushed_release_tag_object=""
  if ! git tag -a "${release_tag}" "${source_revision}" -m "HockeyMONStream ${release_tag}"; then
    remove_unpushed_release_tag push-failed
    trap - EXIT INT TERM
    echo "ERROR: could not create the release tag ${release_tag}." >&2
    return 1
  fi
  # The id has to be read back through the name, so check that what is there
  # is what was just created: a forced replacement in this gap would otherwise
  # be adopted, then pushed under packages built from another commit. Claim it
  # only once it peels to this run's revision; anything else is left untouched
  # rather than deleted, because it is not ours to remove.
  tag_object="$(git rev-parse -q --verify "refs/tags/${release_tag}" || true)"
  tagged_commit="$(git rev-parse -q --verify "${tag_object}^{commit}" 2>/dev/null || true)"
  expected_commit="$(git rev-parse -q --verify "${source_revision}^{commit}" 2>/dev/null || true)"
  if [[ -z "${tag_object}" || -z "${expected_commit}" || "${tagged_commit}" != "${expected_commit}" ]]; then
    unpushed_release_tag=""
    trap - EXIT INT TERM
    echo "ERROR: ${release_tag} does not point at this run's revision after creating it." >&2
    echo "Refusing to push it. Check what that tag is before publishing again." >&2
    return 1
  fi
  unpushed_release_tag_object="${tag_object}"
  # Pushed by object id rather than by name: the name could be replaced
  # between the read-back and the push, which would publish another commit
  # under the packages this run built.
  if ! git push "${remote}" "${unpushed_release_tag_object}:refs/tags/${release_tag}"; then
    remove_unpushed_release_tag push-failed
    trap - EXIT INT TERM
    return 1
  fi
  # On the remote now, so it has to survive a later failure: the release can
  # then be finished by hand or deleted deliberately.
  unpushed_release_tag=""
  unpushed_release_tag_object=""
  trap - EXIT INT TERM
}

# Which collision to report, from the three observed states. Ordered most to
# least published: a release implies its tag, and a pushed tag outlives a
# release creation that failed after it. An indeterminate lookup is reported
# as itself, never as an absence, because each recovery below deletes
# something.
classify_release_tag_collision() {
  local release_state="$1" remote_state="$2" local_state="$3"
  case "${release_state}" in
    published|draft) printf '%s\n' "${release_state}"; return 0 ;;
    unknown) printf 'unknown-release\n'; return 0 ;;
  esac
  case "${remote_state}" in
    present) printf 'remote-tag\n'; return 0 ;;
    unknown) printf 'unknown-remote\n'; return 0 ;;
  esac
  if [[ "${local_state}" == "present" ]]; then
    printf 'local-tag\n'
    return 0
  fi
  printf 'none\n'
}

# Describe one collision on stderr. These messages state what was observed and
# leave the cause to the operator: a tag on origin may belong to a publish
# that is still running, and a local tag may have been created deliberately.
report_release_tag_collision() {
  local kind="$1" tag="$2" repository="$3" stray_commit="$4" head_commit="$5" staging="$6"
  case "${kind}" in
    published)
      echo "ERROR: ${tag} is already released at ${repository}." >&2
      echo "Nothing to retry here. The next version comes from origin's newest release tag," >&2
      echo "so check what origin has: git ls-remote --tags origin" >&2
      ;;
    draft)
      echo "ERROR: ${repository} has a draft release holding the name ${tag}." >&2
      echo "A draft can exist before its tag does, including while a publish is still" >&2
      echo "uploading its assets. Find out whose it is and let a running publish finish;" >&2
      echo "publish or discard an abandoned draft." >&2
      ;;
    remote-tag)
      echo "ERROR: origin has tag ${tag}, but ${repository} has no release for it." >&2
      echo "It appeared after this run listed origin's tags. That is either an earlier run" >&2
      echo "that pushed the tag and then failed to create the release, or a publish still" >&2
      echo "in progress right now, which owns both that tag and the release it is about to" >&2
      echo "create. Check for a running publish first, then look again: a release that has" >&2
      echo "appeared in the meantime is finished, and deleting it would take its assets" >&2
      echo "with it. Only once nothing is using the tag:" >&2
      echo "  make delete-release RELEASE_TAG=${tag}" >&2
      ;;
    local-tag)
      echo "ERROR: tag ${tag} exists in this clone only, on neither origin nor ${repository}." >&2
      echo "It points at ${stray_commit}; a fresh tag would point at HEAD, ${head_commit}." >&2
      echo "An interrupted publish leaves exactly this behind, but it may also have been" >&2
      echo "created deliberately. If it is a leftover, delete it and retry:" >&2
      echo "  git tag -d ${tag}" >&2
      ;;
    unknown-release)
      echo "ERROR: could not determine whether ${repository} already has a release for ${tag}." >&2
      echo "Refusing to guess: every recovery from here deletes something. Retry once" >&2
      echo "'gh release view ${tag} --repo ${repository}' answers." >&2
      ;;
    unknown-remote)
      echo "ERROR: could not determine whether origin already has tag ${tag}." >&2
      echo "Refusing to guess: every recovery from here deletes something. Retry once" >&2
      echo "'git ls-remote --tags origin' answers." >&2
      ;;
    *)
      echo "ERROR: unexpected release tag state: ${kind}" >&2
      ;;
  esac
  if [[ -n "${staging}" ]]; then
    echo "Staged packages from this version are still in:" >&2
    echo "  ${staging}" >&2
    echo "That directory is named after ${tag}, so it blocks a retry of this version but" >&2
    echo "not a run at the next one. Archive or remove it before retrying this one." >&2
  fi
}

# Let the test source the platform selection without running a release.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --exclude=*) EXCLUDE_SPEC+=" ${1#--exclude=}" ;;
    --exclude)
      if [[ $# -lt 2 ]]; then
        echo "ERROR: --exclude requires a platform list." >&2
        usage >&2
        exit 2
      fi
      EXCLUDE_SPEC+=" $2"
      shift
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

included_platform_list="$(resolve_release_platforms "${EXCLUDE_SPEC}")" || exit 2
mapfile -t included_platforms <<< "${included_platform_list}"

publishing() {
  local platform="$1" candidate
  for candidate in "${included_platforms[@]}"; do
    if [[ "${candidate}" == "${platform}" ]]; then
      return 0
    fi
  done
  return 1
}

if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: required command not found: git" >&2
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
# Resolve the repository's former name so release metadata and generated links
# use the current public product URL even from an older local clone.
if [[ "${repository}" == "cjolivier01/hstream" ]]; then
  repository="cjolivier01/HockeyMONStream"
fi

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

excluded_labels=()
for platform in "${RELEASE_PLATFORMS[@]}"; do
  publishing "${platform}" || excluded_labels+=("$(platform_label "${platform}")")
done

echo "Next release: ${release_tag}"
echo "Source commit: ${source_revision}"
if [[ "${#excluded_labels[@]}" -gt 0 ]]; then
  echo "Excluded platforms:"
  printf '  %s\n' "${excluded_labels[@]}"
fi
echo "Artifacts:"
publishing ubuntu24 && echo "  hstream_${release_tag}_ubuntu24.04_amd64.deb"
publishing ubuntu26 && echo "  hstream_${release_tag}_ubuntu26.04_amd64.deb"
if publishing jetson; then
  echo "  hstream_${release_tag}_jetson-ubuntu24.04_arm64.deb"
fi
publishing windows && echo "  hstream_${release_tag}_windows-wsl-setup.exe"
echo "  install-hstream-deb.sh"
echo "  SHA256SUMS"

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "Dry run only; no packages, tags, or GitHub releases were created."
  exit 0
fi

# Only require the toolchain the selected platforms actually build with, so an
# excluded target's tools are not needed to publish the rest.
required_commands=(gh make sha256sum)
if publishing ubuntu24 || publishing ubuntu26 || publishing jetson; then
  required_commands+=(dpkg-deb)
fi
if publishing windows; then
  required_commands+=(makensis rsvg-convert icotool file osslsigncode)
fi
for command_name in "${required_commands[@]}"; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "ERROR: required release build command not found: ${command_name}" >&2
    exit 1
  fi
done
if publishing windows; then
  if [[ ! -f "${WINDOWS_SIGNING_PKCS12:-}" || ! -f "${WINDOWS_SIGNING_PASSWORD_FILE:-}" ]]; then
    echo "ERROR: publishing the Windows installer requires WINDOWS_SIGNING_PKCS12 and WINDOWS_SIGNING_PASSWORD_FILE." >&2
    exit 1
  fi
  if [[ -n "${WINDOWS_SIGNING_CA_FILE:-}" && ! -f "${WINDOWS_SIGNING_CA_FILE}" ]]; then
    echo "ERROR: WINDOWS_SIGNING_CA_FILE must name a readable PEM certificate file." >&2
    exit 1
  fi
fi
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: GitHub CLI is not authenticated. Run 'gh auth login' first." >&2
  exit 1
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
# The next version comes from origin alone, so a tag that exists anywhere else
# collides without ever advancing the number. The places it can exist have
# unrelated causes and different fixes, so report the one that actually fired
# rather than the bare collision.
local_tag_state=absent
if git show-ref --verify --quiet "refs/tags/${release_tag}"; then
  local_tag_state=present
fi
tag_collision="$(classify_release_tag_collision \
  "$(github_release_state "${repository}" "${release_tag}")" \
  "$(remote_tag_state origin "${release_tag}")" \
  "${local_tag_state}")"
if [[ "${tag_collision}" != "none" ]]; then
  stray_commit=""
  if [[ "${local_tag_state}" == "present" ]]; then
    stray_commit="$(git rev-parse --short "refs/tags/${release_tag}^{commit}" 2>/dev/null \
      || echo "an unreadable commit")"
  fi
  report_release_tag_collision "${tag_collision}" "${release_tag}" "${repository}" \
    "${stray_commit}" "$(git rev-parse --short HEAD)" \
    "$([[ -e "${release_dir}" ]] && printf '%s' "${release_dir}")"
  exit 1
fi
if [[ -e "${release_dir}" ]]; then
  echo "ERROR: release staging directory already exists: ${release_dir}" >&2
  echo "Remove or archive it explicitly before retrying." >&2
  exit 1
fi

build_dir="${release_dir}/build"
mkdir -p "${release_dir}"

if publishing ubuntu24; then
  mkdir -p "${build_dir}/ubuntu24.04"
  make deb-ubuntu24 PACKAGE_VERSION="${release_tag}" DEB_OUTPUT_DIR="${build_dir}/ubuntu24.04"
fi
if publishing ubuntu26; then
  mkdir -p "${build_dir}/ubuntu26.04"
  make deb-ubuntu26 PACKAGE_VERSION="${release_tag}" DEB_OUTPUT_DIR="${build_dir}/ubuntu26.04"
fi
if publishing jetson; then
  mkdir -p "${build_dir}/jetson"
  make deb-jetson PACKAGE_VERSION="${release_tag}" JETSON_DEB_OUTPUT_DIR="${build_dir}/jetson"
fi
if publishing windows; then
  make windows-installer \
    WINDOWS_INSTALLER_VERSION="${release_tag}" \
    WINDOWS_INSTALLER_OUTPUT_DIR="${build_dir}/windows" \
    WINDOWS_INSTALLER_REPOSITORY="${repository}" \
    WINDOWS_SIGNING_PKCS12="${WINDOWS_SIGNING_PKCS12}" \
    WINDOWS_SIGNING_PASSWORD_FILE="${WINDOWS_SIGNING_PASSWORD_FILE}" \
    WINDOWS_SIGNING_CA_FILE="${WINDOWS_SIGNING_CA_FILE:-}" \
    WINDOWS_SIGNING_TIMESTAMP_URL="${WINDOWS_SIGNING_TIMESTAMP_URL:-http://timestamp.digicert.com}"
fi

# Names of the staged assets, in publication order. Globbing the staging
# directory would break as soon as a platform drops a whole file class.
staged_assets=()

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
  staged_assets+=("${published_name}")
}

if publishing ubuntu24; then
  validate_and_stage_deb \
    "${build_dir}/ubuntu24.04/hstream_${package_version}_amd64.deb" \
    amd64 24.04 desktop "hstream_${release_tag}_ubuntu24.04_amd64.deb"
fi
if publishing ubuntu26; then
  validate_and_stage_deb \
    "${build_dir}/ubuntu26.04/hstream_${package_version}_amd64.deb" \
    amd64 26.04 desktop "hstream_${release_tag}_ubuntu26.04_amd64.deb"
fi
if publishing jetson; then
  validate_and_stage_deb \
    "${build_dir}/jetson/hstream_${package_version}_arm64.deb" \
    arm64 24.04 jetson "hstream_${release_tag}_jetson-ubuntu24.04_arm64.deb"
fi
if publishing windows; then
  windows_installer="${build_dir}/windows/hstream_${release_tag}_windows-wsl-setup.exe"
  if [[ ! -f "${windows_installer}" ]] ||
     [[ "$(file -Lb "${windows_installer}")" != *"PE32 executable"* ]]; then
    echo "ERROR: expected Windows WSL bootstrapper was not created: ${windows_installer}" >&2
    exit 1
  fi
  windows_verify_args=(-in "${windows_installer}")
  if [[ -n "${WINDOWS_SIGNING_CA_FILE:-}" ]]; then
    windows_verify_args=(-CAfile "${WINDOWS_SIGNING_CA_FILE}" "${windows_verify_args[@]}")
  fi
  if ! osslsigncode verify "${windows_verify_args[@]}"; then
    echo "ERROR: Windows WSL bootstrapper is not validly Authenticode-signed: ${windows_installer}" >&2
    exit 1
  fi
  install -m 0755 "${windows_installer}" \
    "${release_dir}/hstream_${release_tag}_windows-wsl-setup.exe"
  staged_assets+=("hstream_${release_tag}_windows-wsl-setup.exe")
fi

# Publish the exact desktop dependency installer from this tag. It validates
# the local DeepStream and HStream packages and handles Ubuntu compatibility.
desktop_installer_asset="${release_dir}/install-hstream-deb.sh"
git show "${source_revision}:scripts/install_deb.sh" > "${desktop_installer_asset}"
chmod 0755 "${desktop_installer_asset}"
staged_assets+=("install-hstream-deb.sh")

if [[ "${#staged_assets[@]}" -eq 0 ]]; then
  echo "ERROR: no release assets were staged." >&2
  exit 1
fi
# Built by hand rather than with "${staged_assets[@]/#/./}": since bash 5.2's
# patsub_replacement an unquoted & in the replacement expands to the matched
# text, so a staging path like /srv/r&d would name files that do not exist.
checksum_names=()
for asset in "${staged_assets[@]}"; do
  checksum_names+=("./${asset}")
done
(
  cd "${release_dir}"
  sha256sum "${checksum_names[@]}" > SHA256SUMS
)

# Recheck after the long builds so a concurrent publisher cannot claim the
# same version while this process was compiling packages.
git fetch --quiet origin "${default_branch}"
new_latest_tag="$(latest_remote_release_tag)"
# Same rule as the preflight: a lookup that could not answer is not an
# absence. Tagging on an unanswered question is how two publishers end up
# claiming one version.
recheck_collision="$(classify_release_tag_collision \
  "$(github_release_state "${repository}" "${release_tag}")" \
  "$(remote_tag_state origin "${release_tag}")" \
  "$(git show-ref --verify --quiet "refs/tags/${release_tag}" && echo present || echo absent)")"
revisions_changed=0
if [[ "${new_latest_tag}" != "${latest_tag}" ]] ||
   [[ "$(git rev-parse HEAD)" != "${source_revision}" ]] ||
   [[ "$(git rev-parse "refs/remotes/origin/${default_branch}")" != "${remote_revision}" ]]; then
  revisions_changed=1
fi
if [[ "${revisions_changed}" -eq 1 || "${recheck_collision}" != "none" ]]; then
  # Only claim a change when one was seen: an unanswered lookup stops the run
  # too, and reporting that as a changed revision sends the reader hunting
  # for something that did not happen.
  if [[ "${revisions_changed}" -eq 1 ]]; then
    echo "ERROR: release tags or source revisions changed while packages were building." >&2
  fi
  if [[ "${recheck_collision}" != "none" ]]; then
    report_release_tag_collision "${recheck_collision}" "${release_tag}" "${repository}" \
      "$(git rev-parse --short "refs/tags/${release_tag}^{commit}" 2>/dev/null || echo "an unreadable commit")" \
      "$(git rev-parse --short HEAD)" ""
  fi
  echo "The packages this run built remain in ${release_dir}." >&2
  exit 1
fi

if ! push_release_tag "${release_tag}" "${source_revision}" origin; then
  echo "ERROR: could not push release tag ${release_tag}." >&2
  exit 1
fi

release_assets=()
for asset in "${staged_assets[@]}"; do
  release_assets+=("${release_dir}/${asset}")
done
release_assets+=("${release_dir}/SHA256SUMS")
release_notes=$'HockeyMONStream is source-available software for stitching two sports-camera videos, automatically tracking the play, and producing a virtual pan-and-zoom program view. It runs without a HockeyMONStream subscription fee on supported NVIDIA GPU hardware. The repository has mixed file-level licensing.\n\n[Product overview](https://cjolivier01.github.io/HockeyMONStream/) · [Installation guide](https://cjolivier01.github.io/HockeyMONStream/install.html) · [Source and licensing](https://github.com/cjolivier01/HockeyMONStream)'
# A reader cannot tell a deliberately reduced matrix from a broken upload, so
# name the missing targets instead of leaving a gap in the asset list.
if [[ "${#excluded_labels[@]}" -gt 0 ]]; then
  release_notes+=$'\n\n## Platforms not in this release\n\n> [!NOTE]\n'
  release_notes+='> This release does not include packages for '
  printf -v excluded_sentence '%s, ' "${excluded_labels[@]}"
  release_notes+="${excluded_sentence%, }"
  release_notes+=$'. Use the most recent release that lists those assets.'
fi
if publishing windows && [[ -n "${WINDOWS_SIGNING_CA_FILE:-}" ]]; then
  private_release_warning=$'## Windows installer trust\n\n> [!WARNING]\n'
  private_release_warning+=$'> The Windows installer in this release uses a private/self-signed publisher certificate. '
  private_release_warning+=$'Windows will report an unknown publisher unless that certificate is explicitly trusted on '
  private_release_warning+=$'the target machine. The Debian artifacts are unaffected.'
  release_notes+=$'\n\n'
  release_notes+="${private_release_warning}"
fi
if ! gh release create "${release_tag}" "${release_assets[@]}" \
    --repo "${repository}" --verify-tag --generate-notes --notes "${release_notes}" \
    --title "HockeyMONStream ${release_tag} — multi-camera sports video"; then
  echo "ERROR: tag ${release_tag} was pushed, but GitHub release publication failed." >&2
  echo "After inspecting the failure, retry publication or run:" >&2
  echo "  make delete-release RELEASE_TAG=${release_tag}" >&2
  exit 1
fi

echo "Published HockeyMONStream ${release_tag}:"
if ! gh release view "${release_tag}" --repo "${repository}" --json url --jq '.url'; then
  echo "WARNING: release publication succeeded, but its URL could not be queried." >&2
fi
