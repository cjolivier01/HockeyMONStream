#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/publish_release.sh
source "${SCRIPT_DIR}/publish_release.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
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

echo "PASS"
