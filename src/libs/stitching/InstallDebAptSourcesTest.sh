#!/bin/bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "FAIL: expected the installer path" >&2
  exit 1
fi

# shellcheck disable=SC1090 # Bazel supplies the production installer path.
HSTREAM_INSTALLER_SOURCE_ONLY=1 source "$1"

test_root="$(mktemp -d /tmp/hstream-apt-sources-test.XXXXXX)"
cleanup_test() {
  rm -rf "${test_root}"
  if [[ -n "${compat_source_transition_dir:-}" ]]; then rm -rf "${compat_source_transition_dir}"; fi
}
trap cleanup_test EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

reset_apt_tree() {
  rm -rf "${test_root:?}/etc" "${test_root}/source-targets"
  mkdir -p "${test_root}/etc/apt/sources.list.d"
  DISABLED_CUDA_SOURCE_COUNT=0
}

compat_uri='https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/'

# A disabled source stays untouched.  An active deb-src-only source is not a
# usable provider and is disabled before the canonical binary source appears.
reset_apt_tree
printf '%s\n' \
  'Types: deb' \
  "URIs: ${compat_uri}" \
  'Suites: /' \
  'Enabled: no' \
  'Signed-By: /disabled.gpg' \
  >"${test_root}/etc/apt/sources.list.d/inactive.sources"
printf '%s\n' \
  'Types: deb-src' \
  "URIs: ${compat_uri}" \
  'Suites: /' \
  'Signed-By: /source-only.gpg' \
  >"${test_root}/etc/apt/sources.list.d/source-only.sources"
inactive_before="$(sha256sum "${test_root}/etc/apt/sources.list.d/inactive.sources")"
disable_cuda_compat_sources "${test_root}"
[[ "${DISABLED_CUDA_SOURCE_COUNT}" -eq 1 ]] || fail "deb-src-only source was treated as a usable provider"
[[ "$(sha256sum "${test_root}/etc/apt/sources.list.d/inactive.sources")" == "${inactive_before}" ]] || \
  fail "disabled Deb822 stanza was modified"
grep -qFx 'Enabled: no' "${test_root}/etc/apt/sources.list.d/source-only.sources" || \
  fail "deb-src-only stanza was not disabled"
publish_cuda_compat_source "${test_root}"
grep -qFx "deb [arch=amd64 signed-by=${CUDA_COMPAT_KEYRING}] ${compat_uri} /" \
  "${test_root}${CUDA_COMPAT_SOURCE}" || fail "canonical binary source was not published"

# URI spelling and architecture restrictions cannot suppress the canonical
# source.  Exact flat-suite duplicates are commented; another suite remains.
reset_apt_tree
printf '%s\n' \
  'deb [arch=arm64 signed-by=/wrong-arch.gpg] http://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64 /' \
  'deb https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
  'deb-src https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
  'deb [signed-by=/other-suite.gpg] https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ stable' \
  >"${test_root}/etc/apt/sources.list.d/variants.list"
disable_cuda_compat_sources "${test_root}"
[[ "${DISABLED_CUDA_SOURCE_COUNT}" -eq 3 ]] || fail "exact-suite URI variants were not all disabled"
[[ "$(grep -c '^# HStream disabled duplicate CUDA compatibility source:' \
  "${test_root}/etc/apt/sources.list.d/variants.list")" -eq 3 ]] || fail "list duplicates were not marked"
grep -qF 'deb [signed-by=/other-suite.gpg]' "${test_root}/etc/apt/sources.list.d/variants.list" || \
  fail "different repository suite was disabled"
publish_cuda_compat_source "${test_root}"
grep -qF 'arch=amd64' "${test_root}${CUDA_COMPAT_SOURCE}" || fail "wrong-architecture entry suppressed amd64 source"

# Deb822 fields may continue on following lines.  A wrong-architecture active
# stanza is still disabled because Signed-By conflicts are repository-wide;
# the canonical source supplies the usable amd64 provider.
reset_apt_tree
printf '%s\n' \
  'Types:' \
  ' deb deb-src' \
  'URIs:' \
  ' https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64' \
  'Suites:' \
  ' /' \
  'Architectures: arm64' \
  'Signed-By:' \
  ' /old-continuation.gpg' \
  >"${test_root}/etc/apt/sources.list.d/continued.sources"
disable_cuda_compat_sources "${test_root}"
[[ "${DISABLED_CUDA_SOURCE_COUNT}" -eq 1 ]] || fail "continued Deb822 source was not recognized"
grep -qFx 'Enabled: no' "${test_root}/etc/apt/sources.list.d/continued.sources" || \
  fail "continued Deb822 source was not disabled"
grep -qFx 'Architectures: arm64' "${test_root}/etc/apt/sources.list.d/continued.sources" || \
  fail "Deb822 architecture field was lost"
grep -qF '/old-continuation.gpg' "${test_root}/etc/apt/sources.list.d/continued.sources" || \
  fail "disabled Deb822 source was destructively rewritten"

# Source-file symlinks are followed without replacing the symlink itself.
reset_apt_tree
mkdir -p "${test_root}/source-targets"
printf '%s\n' "deb [signed-by=/old-link.gpg] ${compat_uri} /" >"${test_root}/source-targets/cuda.list"
ln -s "../../../source-targets/cuda.list" "${test_root}/etc/apt/sources.list.d/cuda-link.list"
disable_cuda_compat_sources "${test_root}"
[[ "${DISABLED_CUDA_SOURCE_COUNT}" -eq 1 ]] || fail "symlinked source was not recognized"
[[ -L "${test_root}/etc/apt/sources.list.d/cuda-link.list" ]] || fail "source symlink was replaced"
grep -q '^# HStream disabled' "${test_root}/source-targets/cuda.list" || fail "symlink target was not disabled"

# A mixed Deb822 stanza cannot be disabled without also removing an unrelated
# repository.  Reject it atomically and leave the file unchanged.
reset_apt_tree
printf '%s\n' \
  'Types: deb' \
  "URIs: ${compat_uri} https://example.invalid/packages" \
  'Suites: /' \
  >"${test_root}/etc/apt/sources.list.d/mixed.sources"
mixed_before="$(sha256sum "${test_root}/etc/apt/sources.list.d/mixed.sources")"
if disable_cuda_compat_sources "${test_root}" 2>/dev/null; then
  fail "unsafe mixed Deb822 stanza was accepted"
fi
[[ "$(sha256sum "${test_root}/etc/apt/sources.list.d/mixed.sources")" == "${mixed_before}" ]] || \
  fail "rejected mixed Deb822 stanza was modified"

# Pre-update recovery removes the uniquely owned HStream source but edits only
# the matching line in NVIDIA's legacy conffile.  A normal failure restores the
# exact prior files; a completed transition preserves unrelated conffile data.
reset_apt_tree
printf '%s\n' 'new managed source' >"${test_root}${CUDA_COMPAT_SOURCE}"
printf '%s\n' \
  '# locally retained comment' \
  "deb [signed-by=/legacy.gpg] ${compat_uri} /" \
  'deb [signed-by=/unrelated.gpg] https://example.invalid/packages stable' \
  >"${test_root}${CUDA_LEGACY_COMPAT_SOURCE}"
printf '%s\n' 'unrelated source' >"${test_root}/etc/apt/sources.list.d/unrelated.list"
managed_before="$(sha256sum "${test_root}${CUDA_COMPAT_SOURCE}")"
legacy_before="$(sha256sum "${test_root}${CUDA_LEGACY_COMPAT_SOURCE}")"
(
  begin_compat_source_transition
  disable_installer_managed_cuda_sources "${test_root}"
  disable_cuda_compat_sources "${test_root}" "${CUDA_LEGACY_COMPAT_SOURCE}"
  [[ ! -e "${test_root}${CUDA_COMPAT_SOURCE}" ]] || fail "current managed source survived pre-update repair"
  grep -q '^# HStream disabled duplicate' "${test_root}${CUDA_LEGACY_COMPAT_SOURCE}" || \
    fail "legacy compatibility line was not disabled"
  grep -qF 'https://example.invalid/packages' "${test_root}${CUDA_LEGACY_COMPAT_SOURCE}" || \
    fail "unrelated legacy conffile entry was removed"
  restore_compat_source_transition
  rm -rf "${compat_source_transition_dir}"
)
[[ "$(sha256sum "${test_root}${CUDA_COMPAT_SOURCE}")" == "${managed_before}" ]] || \
  fail "normal-failure recovery did not restore the managed source"
[[ "$(sha256sum "${test_root}${CUDA_LEGACY_COMPAT_SOURCE}")" == "${legacy_before}" ]] || \
  fail "normal-failure recovery did not restore the legacy conffile"

# A failed later backup must not register partial metadata or prevent an
# earlier mutation from being restored during EXIT cleanup.
(
  first_source="${test_root}/etc/apt/sources.list.d/first-to-restore.list"
  second_source="${test_root}/etc/apt/sources.list.d/second-backup-fails.list"
  printf '%s\n' 'first source' >"${first_source}"
  printf '%s\n' 'second source' >"${second_source}"
  begin_compat_source_transition
  backup_compat_source_path "${first_source}"
  rm -f "${first_source}"
  # shellcheck disable=SC2329 # Invoked indirectly by the sourced production function.
  cp() { return 1; }
  set +e
  backup_compat_source_path "${second_source}" 2>/dev/null
  backup_status=$?
  set -e
  unset -f cp
  [[ "${backup_status}" -ne 0 ]] || fail "injected second backup unexpectedly succeeded"
  restore_compat_source_transition
  [[ "$(<"${first_source}")" == 'first source' ]] || \
    fail "earlier source was not restored after a later backup failure"
  rm -rf "${compat_source_transition_dir}"
)

begin_compat_source_transition
disable_installer_managed_cuda_sources "${test_root}"
disable_cuda_compat_sources "${test_root}" "${CUDA_LEGACY_COMPAT_SOURCE}"
disable_cuda_compat_sources "${test_root}"
publish_cuda_compat_source "${test_root}"
commit_compat_source_transition
[[ "$(<"${test_root}/etc/apt/sources.list.d/unrelated.list")" == 'unrelated source' ]] || \
  fail "pre-update repair changed an unrelated source"
grep -qF '# locally retained comment' "${test_root}${CUDA_LEGACY_COMPAT_SOURCE}" || \
  fail "completed transition lost legacy conffile comments"
grep -qF 'https://example.invalid/packages' "${test_root}${CUDA_LEGACY_COMPAT_SOURCE}" || \
  fail "completed transition lost an unrelated legacy conffile entry"
[[ -s "${test_root}${CUDA_COMPAT_SOURCE}" ]] || fail "canonical source was not recoverable after interruption"

# A repository failure APT attributes to a third party is reported and
# tolerated; one that names a required repository, or no repository at all,
# still fails the installation.
apt_update_output=""
apt_update_status=0
# shellcheck disable=SC2329 # Invoked indirectly by the sourced production function.
apt-get() {
  [[ "${1:-}" == update ]] || fail "unexpected apt-get invocation: $*"
  # APT writes its diagnostics to stderr and its progress to stdout.  Split
  # them the same way, so that dropping the production redirection that merges
  # them into the classified log fails these cases instead of passing.
  local line
  while IFS= read -r line; do
    case "${line}" in
      'E: '* | 'W: '*) printf '%s\n' "${line}" >&2 ;;
      *) printf '%s\n' "${line}" ;;
    esac
  done <<<"${apt_update_output}"
  return "${apt_update_status}"
}

# Give the installer a private temporary directory so the leak check below is
# exact: counting /tmp would race with any other run on the same machine.
export TMPDIR="${test_root}/tmp"
mkdir -p "${TMPDIR}"
update_errors="${test_root}/update.err"
run_update_as() {
  # An empty first argument is the first call site, which requires nothing.
  local required="$1"
  shift
  apt_update_output="$(printf '%s\n' "$@")"
  apt_update_status=100
  set +e
  if [[ -n "${required}" ]]; then
    run_apt_update "${required}" >/dev/null 2>"${update_errors}"
  else
    run_apt_update >/dev/null 2>"${update_errors}"
  fi
  update_status=$?
  set -e
}
run_update() {
  run_update_as "${CUDA_REPOSITORY_PREFIX}" "$@"
}

xilinx_fetch='E: Failed to fetch https://packages.xilinx.com/artifactory/debian-packages/dists/noble/InRelease  401   [IP: 54.188.199.73 443]'
xilinx_signed="E: The repository 'https://packages.xilinx.com/artifactory/debian-packages noble InRelease' is no longer signed."
index_summary='E: Some index files failed to download. They have been ignored, or old ones used instead.'
radeon_warning='W: https://repo.radeon.com/rocm/apt/6.2/dists/noble/InRelease: Key is stored in legacy trusted.gpg keyring'

apt_update_output='Reading package lists...'$'\n'
apt_update_status=0
run_apt_update "${CUDA_REPOSITORY_PREFIX}" >/dev/null 2>&1 || fail "a successful update was reported as a failure"

run_update 'Hit:1 http://us.archive.ubuntu.com/ubuntu noble InRelease' \
  "${xilinx_fetch}" "${xilinx_signed}" "${radeon_warning}" "${index_summary}"
[[ "${update_status}" -eq 0 ]] || fail "an unrelated repository failure aborted the installation"
grep -qF 'https://packages.xilinx.com/artifactory/debian-packages' "${update_errors}" || \
  fail "the unreachable repository was not reported"
if grep -qF '/dists/noble/InRelease' "${update_errors}"; then
  fail "the report named an index file instead of the repository"
fi
[[ "$(grep -c 'packages.xilinx.com' "${update_errors}")" -eq 1 ]] || \
  fail "one repository was reported more than once"

# Flat CUDA repositories are matched through the scheme and trailing slash
# spellings APT may print for them.
run_update 'E: Failed to fetch http://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/InRelease  503' \
  "${index_summary}"
[[ "${update_status}" -ne 0 ]] || fail "a required CUDA repository failure was tolerated"
run_update "E: The repository 'https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64/ InRelease' is not signed." \
  "${xilinx_fetch}"
[[ "${update_status}" -ne 0 ]] || fail "a required CUDA repository failure was masked by a third-party failure"
# Reducing an index URL to its repository must not shorten a required URI
# below its own prefix.
run_update 'E: Failed to fetch https://developer.download.nvidia.com/compute/cuda/repos/dists/noble/InRelease  404  Not Found'
[[ "${update_status}" -ne 0 ]] || fail "a required repository was tolerated after its base was truncated"

# The first call site declares no required repository, because the keyring
# install below it is what repairs an NVIDIA source whose key is missing.  Use
# a CUDA failure, so that hardcoding the required prefix fails this case
# instead of passing it.
run_update_as "" \
  "E: The repository 'https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ InRelease' is not signed." \
  "${index_summary}"
[[ "${update_status}" -eq 0 ]] || fail "an update that requires nothing aborted on the repository it repairs next"

# APT attributes a local fault to whichever download hit it.  The URI does not
# make a full disk the repository's problem, and the installation would hit it
# too.
run_update 'E: Failed to fetch https://packages.xilinx.com/artifactory/debian-packages/dists/noble/main/binary-amd64/Packages.xz  Error writing to file - write (28: No space left on device)' \
  "${index_summary}"
[[ "${update_status}" -ne 0 ]] || fail "a full disk behind a repository URI was tolerated"

# APT reports some fetch failures as warnings.  One against a required
# repository is still fatal; an ordinary warning about the same repository is
# not a failure at all.
run_update "${xilinx_fetch}" \
  "W: Failed to fetch https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/InRelease  Temporary failure resolving 'developer.download.nvidia.com'"
[[ "${update_status}" -ne 0 ]] || fail "a required repository failure reported as a warning was ignored"
run_update "${xilinx_fetch}" \
  'W: https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/InRelease: Key is stored in legacy trusted.gpg keyring' \
  "${index_summary}"
[[ "${update_status}" -eq 0 ]] || fail "an ordinary warning about a required repository failed the installation"

# A capture that did not complete cannot be classified: the log it leaves
# behind may be missing the very line that should have failed the install.
# shellcheck disable=SC2329 # Invoked indirectly by the sourced production function.
tee() {
  command tee "$@"
  return 1
}
run_update "${xilinx_fetch}" "${index_summary}"
unset -f tee
[[ "${update_status}" -ne 0 ]] || fail "a truncated capture was classified as tolerable"

# A local mirror is a repository like any other, and APT names it with a
# transport that carries no authority.
run_update 'E: Failed to fetch file:/srv/local-mirror/dists/noble/InRelease  File not found'
[[ "${update_status}" -eq 0 ]] || fail "an unrelated file: repository failure aborted the installation"
grep -qF 'file:/srv/local-mirror' "${update_errors}" || fail "the file: repository was not reported"

# Failures APT cannot attribute to a repository remain fatal, and are reported
# as themselves rather than as a missing required repository.
run_update 'E: Could not get lock /var/lib/apt/lists/lock. It is held by process 4242'
[[ "${update_status}" -ne 0 ]] || fail "a lock failure was treated as a stale repository"
grep -qF 'Could not get lock' "${update_errors}" || fail "the unattributed failure was not reported"
grep -qF 'status 100' "${update_errors}" || fail "the reported status was not APT's own"
if grep -qF 'repository this installation requires' "${update_errors}"; then
  fail "a lock failure was reported as a required repository failure"
fi
run_update "${xilinx_fetch}" 'E: Could not get lock /var/lib/apt/lists/lock. It is held by process 4242'
[[ "${update_status}" -ne 0 ]] || fail "an unattributed failure was masked by a tolerated one"
run_update "${index_summary}"
[[ "${update_status}" -ne 0 ]] || fail "an unexplained failure was tolerated"

shopt -s nullglob
leftover_logs=("${TMPDIR}"/*)
shopt -u nullglob
[[ "${#leftover_logs[@]}" -eq 0 ]] || \
  fail "apt-get update logs were left behind: ${leftover_logs[*]}"
unset -f apt-get

echo "APT CUDA source reconciliation tests passed"
