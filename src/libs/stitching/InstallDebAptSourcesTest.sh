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

echo "APT CUDA source reconciliation tests passed"
