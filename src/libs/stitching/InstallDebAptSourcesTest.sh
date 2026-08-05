#!/bin/bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "FAIL: expected the installer path" >&2
  exit 1
fi

# shellcheck disable=SC1090 # Bazel supplies the production installer path.
HMSTREAM_INSTALLER_SOURCE_ONLY=1 source "$1"

test_root="$(mktemp -d /tmp/hmstream-apt-sources-test.XXXXXX)"
trap 'rm -rf "${test_root}"' EXIT

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
[[ "$(grep -c '^# HMStream disabled duplicate CUDA compatibility source:' \
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
grep -q '^# HMStream disabled' "${test_root}/source-targets/cuda.list" || fail "symlink target was not disabled"

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

# Pre-update recovery removes both filenames emitted by past installers while
# preserving unrelated source files.  Absence is an APT-valid interrupted state.
reset_apt_tree
printf '%s\n' 'new managed source' >"${test_root}${CUDA_COMPAT_SOURCE}"
printf '%s\n' 'legacy managed source' >"${test_root}${CUDA_LEGACY_COMPAT_SOURCE}"
printf '%s\n' 'unrelated source' >"${test_root}/etc/apt/sources.list.d/unrelated.list"
disable_installer_managed_cuda_sources "${test_root}"
[[ ! -e "${test_root}${CUDA_COMPAT_SOURCE}" ]] || fail "current managed source survived pre-update repair"
[[ ! -e "${test_root}${CUDA_LEGACY_COMPAT_SOURCE}" ]] || fail "legacy managed source survived pre-update repair"
[[ "$(<"${test_root}/etc/apt/sources.list.d/unrelated.list")" == 'unrelated source' ]] || \
  fail "pre-update repair changed an unrelated source"
publish_cuda_compat_source "${test_root}"
[[ -s "${test_root}${CUDA_COMPAT_SOURCE}" ]] || fail "canonical source was not recoverable after interruption"

echo "APT CUDA source reconciliation tests passed"
