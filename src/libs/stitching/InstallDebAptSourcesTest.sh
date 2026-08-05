#!/bin/bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "FAIL: expected the installer path" >&2
  exit 1
fi

# shellcheck disable=SC1090 # Bazel supplies the production installer path.
HMSTREAM_INSTALLER_SOURCE_ONLY=1 source "$1"

test_root="$(mktemp -d /tmp/hmstream-apt-sources-test.XXXXXX)"
cleanup_test() {
  rm -rf "${test_root}"
  if [[ -n "${transaction_dir:-}" ]]; then rm -rf "${transaction_dir}"; fi
}
trap cleanup_test EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

reset_apt_tree() {
  rm -rf "${test_root:?}/etc"
  mkdir -p "${test_root}/etc/apt/sources.list.d"
  NORMALIZED_CUDA_SOURCE_COUNT=0
}

compat_uri='https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/'
test_key='/usr/share/keyrings/hmstream-test-key.gpg'

# Disabled and source-only Deb822 stanzas must not suppress creation of the
# binary compatibility source.
reset_apt_tree
printf '%s\n' \
  'Types: deb' \
  "URIs: ${compat_uri}" \
  'Suites: /' \
  'Enabled: no' \
  'Signed-By: /disabled.gpg' \
  '' \
  'Types: deb-src' \
  "URIs: ${compat_uri}" \
  'Suites: /' \
  'Signed-By: /source-only.gpg' \
  >"${test_root}/etc/apt/sources.list.d/inactive.sources"
inactive_before="$(sha256sum "${test_root}/etc/apt/sources.list.d/inactive.sources")"
normalize_cuda_compat_sources "${test_root}" "${test_key}"
[[ "${NORMALIZED_CUDA_SOURCE_COUNT}" -eq 0 ]] || fail "inactive Deb822 stanzas counted as binary sources"
[[ "$(sha256sum "${test_root}/etc/apt/sources.list.d/inactive.sources")" == "${inactive_before}" ]] || \
  fail "inactive Deb822 stanzas were modified"

# One-line sources are recognized with either slash spelling, canonicalized,
# and assigned one durable key without changing comments or deb-src entries.
reset_apt_tree
printf '%s\n' \
  'deb [arch=amd64 signed-by=/old-one.gpg] http://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64 /' \
  'deb https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
  '# deb https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
  'deb-src https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
  >"${test_root}/etc/apt/sources.list.d/variants.list"
normalize_cuda_compat_sources "${test_root}" "${test_key}"
[[ "${NORMALIZED_CUDA_SOURCE_COUNT}" -eq 2 ]] || fail "URI variants were not both recognized"
[[ "$(grep -cF "deb [arch=amd64 signed-by=${test_key}] ${compat_uri} /" "${test_root}/etc/apt/sources.list.d/variants.list")" -eq 1 ]] || \
  fail "list options were not normalized"
[[ "$(grep -cF "deb [signed-by=${test_key}] ${compat_uri} /" "${test_root}/etc/apt/sources.list.d/variants.list")" -eq 1 ]] || \
  fail "slashless list URI was not normalized"
grep -qF 'deb-src https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
  "${test_root}/etc/apt/sources.list.d/variants.list" || fail "deb-src entry was modified"
if grep -qF '/old-one.gpg' "${test_root}/etc/apt/sources.list.d/variants.list"; then
  fail "old Signed-By remained in an active list entry"
fi

# Deb822 fields may continue on following lines.  The complete active stanza
# is rewritten while retaining its unrelated fields.
reset_apt_tree
printf '%s\n' \
  'Types:' \
  ' deb deb-src' \
  'URIs:' \
  ' https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64' \
  'Suites: /' \
  'Architectures: amd64' \
  'Signed-By:' \
  ' /old-continuation.gpg' \
  >"${test_root}/etc/apt/sources.list.d/continued.sources"
normalize_cuda_compat_sources "${test_root}" "${test_key}"
[[ "${NORMALIZED_CUDA_SOURCE_COUNT}" -eq 1 ]] || fail "continued Deb822 source was not recognized"
grep -qFx "URIs: ${compat_uri}" "${test_root}/etc/apt/sources.list.d/continued.sources" || \
  fail "continued Deb822 URI was not canonicalized"
grep -qFx "Signed-By: ${test_key}" "${test_root}/etc/apt/sources.list.d/continued.sources" || \
  fail "continued Deb822 Signed-By was not replaced"
grep -qFx 'Architectures: amd64' "${test_root}/etc/apt/sources.list.d/continued.sources" || \
  fail "unrelated Deb822 field was lost"
if grep -qF '/old-continuation.gpg' "${test_root}/etc/apt/sources.list.d/continued.sources"; then
  fail "continued old Signed-By value remained"
fi

# Source-file symlinks are followed without replacing the symlink itself.
reset_apt_tree
mkdir -p "${test_root}/source-targets"
printf '%s\n' "deb [signed-by=/old-link.gpg] ${compat_uri} /" >"${test_root}/source-targets/cuda.list"
ln -s "../../../source-targets/cuda.list" "${test_root}/etc/apt/sources.list.d/cuda-link.list"
normalize_cuda_compat_sources "${test_root}" "${test_key}"
[[ "${NORMALIZED_CUDA_SOURCE_COUNT}" -eq 1 ]] || fail "symlinked source was not recognized"
[[ -L "${test_root}/etc/apt/sources.list.d/cuda-link.list" ]] || fail "source symlink was replaced"
grep -qF "signed-by=${test_key}" "${test_root}/source-targets/cuda.list" || fail "symlink target was not updated"

# A mixed Deb822 stanza cannot safely use a release-specific key for an
# unrelated repository.  Reject it without changing the file.
reset_apt_tree
printf '%s\n' \
  'Types: deb' \
  "URIs: ${compat_uri} https://example.invalid/packages" \
  'Suites: /' \
  >"${test_root}/etc/apt/sources.list.d/mixed.sources"
mixed_before="$(sha256sum "${test_root}/etc/apt/sources.list.d/mixed.sources")"
if normalize_cuda_compat_sources "${test_root}" "${test_key}" 2>/dev/null; then
  fail "unsafe mixed Deb822 stanza was accepted"
fi
[[ "$(sha256sum "${test_root}/etc/apt/sources.list.d/mixed.sources")" == "${mixed_before}" ]] || \
  fail "rejected mixed Deb822 stanza was modified"

# Every persistent source/key mutation is recoverable.  This models a failure
# after source reconciliation or cuda-keyring installation.
reset_apt_tree
legacy_source="${test_root}${CUDA_COMPAT_SOURCE}"
package_key="${test_root}/usr/share/keyrings/cuda-archive-keyring.gpg"
custom_key="${test_root}${CUDA_COMPAT_KEYRING}"
mkdir -p "$(dirname "${package_key}")"
printf '%s\n' 'legacy managed source' >"${legacy_source}"
printf '%s\n' 'old package key' >"${package_key}"
transaction_dir=""
# shellcheck disable=SC2034 # Read by functions sourced from the installer.
transaction_committed=0
# shellcheck disable=SC2034 # Read by functions sourced from the installer.
transaction_paths=()
# shellcheck disable=SC2034 # Read by functions sourced from the installer.
transaction_backups=()
# shellcheck disable=SC2034 # Read by functions sourced from the installer.
transaction_existed=()
begin_transaction
disable_installer_managed_cuda_source "${test_root}"
[[ ! -e "${legacy_source}" ]] || fail "installer-managed source was not disabled before APT update"
transaction_backup_path "${package_key}"
printf '%s\n' 'new package key' >"${package_key}"
transaction_backup_path "${custom_key}"
printf '%s\n' 'new custom key' >"${custom_key}"
rollback_transaction
[[ "$(<"${legacy_source}")" == 'legacy managed source' ]] || fail "managed source rollback failed"
[[ "$(<"${package_key}")" == 'old package key' ]] || fail "package key rollback failed"
[[ ! -e "${custom_key}" ]] || fail "new compatibility key survived rollback"

echo "APT CUDA source reconciliation tests passed"
