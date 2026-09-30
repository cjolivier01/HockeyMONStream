#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/deploy.sh
source "${SCRIPT_DIR}/deploy.sh"

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

expect_equal "$(classify_target ubuntu 24.04 x86_64 desktop)" "desktop-ubuntu24.04-amd64" \
  "Ubuntu 24 desktop classification"
expect_equal "$(classify_target ubuntu 26.04 x86_64 desktop)" "desktop-ubuntu26.04-amd64" \
  "Ubuntu 26 desktop classification"
expect_equal "$(classify_target ubuntu 22.04 aarch64 jetson)" "jetson-ubuntu22.04-arm64" \
  "Jetson classification"
if classify_target ubuntu 22.04 x86_64 desktop >/dev/null; then
  fail "unsupported Ubuntu 22 desktop was accepted"
fi
if classify_target ubuntu 24.04 aarch64 desktop >/dev/null; then
  fail "unsupported SBSA desktop was accepted"
fi

expect_equal "$(ubuntu_release ubuntu 24.04 'debian' noble)" 24.04 "native Ubuntu release"
expect_equal "$(ubuntu_release neon 24.04 'ubuntu debian' noble)" 24.04 "KDE neon base release"
expect_equal "$(ubuntu_release pop 22.04 'ubuntu debian' jammy)" 22.04 "Pop!_OS base release"
expect_equal "$(ubuntu_release linuxmint 22 'ubuntu debian' noble)" 24.04 \
  "derivative VERSION_ID is ignored in favor of UBUNTU_CODENAME"
if ubuntu_release debian 12 'debian' bookworm >/dev/null; then
  fail "non-Ubuntu distribution was accepted"
fi
if ubuntu_release neon 24.04 'ubuntu debian' '' >/dev/null; then
  fail "derivative without UBUNTU_CODENAME was accepted"
fi
if ubuntu_release neon 24.04 'ubuntu debian' plucky >/dev/null; then
  fail "derivative on an unpackaged Ubuntu base was accepted"
fi
expect_equal "$(classify_target ubuntu "$(ubuntu_release neon 24.04 'ubuntu debian' noble)" \
  x86_64 desktop)" "desktop-ubuntu24.04-amd64" "KDE neon desktop classification"

# install_deb.sh is copied to each node on its own, so it cannot source this
# file and has to carry a second copy of the codename table.  Pin the two
# together: extract the installer's table and require ubuntu_release() to agree
# on every entry, then require the tables to be the same size so an entry added
# to one but not the other fails here instead of on a user's machine.
installer_codenames="$(sed -n 's/^ *\([a-z]*\)) UBUNTU_RELEASE=\([0-9.]*\) ;;$/\1 \2/p' \
  "${SCRIPT_DIR}/install_deb.sh")"
[[ -n "${installer_codenames}" ]] || fail "no codename table found in install_deb.sh"
while read -r codename release; do
  expect_equal "$(ubuntu_release derivative ignored 'ubuntu debian' "${codename}")" "${release}" \
    "install_deb.sh maps ${codename} the same way deploy.sh does"
done <<< "${installer_codenames}"
expect_equal "$(sed -n "s/^ *\([a-z]*\)) printf '\([0-9.]*\)' ;;$/\1/p" "${SCRIPT_DIR}/deploy.sh" | wc -l)" \
  "$(printf '%s\n' "${installer_codenames}" | wc -l)" \
  "deploy.sh and install_deb.sh cover the same number of Ubuntu bases"

# scripts/deepstream_artifact.sh is shared with make_deb_docker.sh, so both
# entry points find a hand-downloaded artifact in the same places.
deepstream_dirs="$(TOPDIR=/src/repo HOME=/home/tester \
  HSTREAM_DEEPSTREAM_CACHE='' deepstream_artifact_search_dirs /src/repo/dist)"
expect_equal "${deepstream_dirs}" \
  "/src/DeepStream/artifacts
/src/repo/dist
/home/tester/Downloads
/home/tester" "default DeepStream search path"
expect_equal "$(TOPDIR=/repo HOME=/home/tester \
  HSTREAM_DEEPSTREAM_CACHE=/srv/debs deepstream_artifact_search_dirs /repo/dist | head -n 1)" /srv/debs \
  "HSTREAM_DEEPSTREAM_CACHE is searched first"
expect_equal "$(TOPDIR=/src/repo HOME='' HSTREAM_DEEPSTREAM_CACHE='' \
  deepstream_artifact_search_dirs /src/repo/dist)" \
  "/src/DeepStream/artifacts
/src/repo/dist" "search path without HOME"

# The newest supported artifact wins, so an old download left in ~/Downloads
# does not shadow a fresh one in the sibling checkout.  A candidate is judged
# on its control file, not its filename.
if ! command -v dpkg-deb >/dev/null; then
  echo "deploy_test: SKIPPING DeepStream resolver checks (dpkg-deb unavailable)" >&2
else
  fixture_root="$(mktemp -d)"
  trap 'rm -rf "${fixture_root}"' EXIT
  make_deepstream_fixture() {
    local path="$1" version="$2" architecture="$3"
    local root="${fixture_root}/build/$(basename "${path}")"
    mkdir -p "${root}/DEBIAN" "$(dirname "${path}")"
    cat >"${root}/DEBIAN/control" <<CONTROL
Package: deepstream-9.1
Version: ${version}
Architecture: ${architecture}
Maintainer: test <test@example.invalid>
Description: deploy_test fixture
CONTROL
    dpkg-deb --build --root-owner-group "${root}" "${path}" >/dev/null
  }

  fixture_home="${fixture_root}/home"
  fixture_topdir="${fixture_root}/tree/repo"
  fixture_sibling="${fixture_root}/tree/DeepStream/artifacts"
  mkdir -p "${fixture_topdir}"
  make_deepstream_fixture "${fixture_home}/Downloads/deepstream-9.1_9.1.0-1_amd64.deb" 9.1.0-1 amd64
  make_deepstream_fixture "${fixture_sibling}/deepstream-9.1_9.1.1-1_amd64.deb" 9.1.1-1 amd64
  make_deepstream_fixture "${fixture_home}/deepstream-9.1_9.1.9-1_amd64.deb" 9.1.9-1 arm64
  make_deepstream_fixture "${fixture_home}/deepstream-9.1_9.2.0-1_amd64.deb" 9.2.0-1 amd64

  # Not $(...): the resolver reports through a global, which a subshell loses.
  TOPDIR="${fixture_topdir}" HOME="${fixture_home}" HSTREAM_DEEPSTREAM_CACHE='' \
    deepstream_artifact_resolve test '' hint "${fixture_topdir}/dist" \
    >"${fixture_root}/resolve.log" || fail "resolver rejected a usable artifact"
  resolve_output="$(cat "${fixture_root}/resolve.log")"
  expect_equal "${DEEPSTREAM_ARTIFACT}" "${fixture_sibling}/deepstream-9.1_9.1.1-1_amd64.deb" \
    "newest supported amd64 artifact wins"
  expect_equal "${resolve_output}" "[test] Using DeepStream 9.1.1-1 from ${DEEPSTREAM_ARTIFACT}" \
    "resolver reports what it picked"

  if TOPDIR="${fixture_root}/bare/repo" HOME="${fixture_root}/bare/home" \
    HSTREAM_DEEPSTREAM_CACHE='' deepstream_artifact_resolve test '' hint \
    "${fixture_root}/bare/repo/dist" >/dev/null 2>&1; then
    fail "resolver accepted a tree with no usable artifact"
  fi
  if TOPDIR="${fixture_topdir}" deepstream_artifact_resolve test \
    "${fixture_home}/deepstream-9.1_9.2.0-1_amd64.deb" hint >/dev/null 2>&1; then
    fail "resolver accepted an out-of-range explicit artifact"
  fi

  rm -rf "${fixture_root}"
  trap - EXIT
fi

parse_nodes " monster,stubby,user@mini "
expect_equal "${#NODES_LIST[@]}" 3 "node count"
expect_equal "${NODES_LIST[0]}" monster "first node"
expect_equal "${NODES_LIST[2]}" user@mini "last node"
if parse_nodes "monster,,mini" >/dev/null 2>&1; then
  fail "empty node was accepted"
fi
if parse_nodes "monster," >/dev/null 2>&1; then
  fail "trailing empty node was accepted"
fi
if parse_nodes "monster,monster" >/dev/null 2>&1; then
  fail "duplicate node was accepted"
fi
if parse_nodes "-bad" >/dev/null 2>&1; then
  fail "option-like node was accepted"
fi

OPERATION=undeploy
if parse_nodes "" >/dev/null 2>&1; then
  fail "undeploy accepted a missing NODES value"
fi
OPERATION=deploy

expect_equal "$(normalize_package_version v1.2.3)" 1.2.3 "version prefix removal"
expect_equal "$(normalize_package_version feature/test)" 0.0+git.feature.test "version normalization"
expect_equal "$(deployment_action '' 1.2.3)" installed "new install action"
expect_equal "$(deployment_action 1.2.3 1.2.3)" reinstalled "same version action"
expect_equal "$(deployment_action 1.2.2 1.2.3)" updated "upgrade action"
expect_equal "$(deployment_action 1.2.4 1.2.3)" downgraded "downgrade action"
expect_equal "$(deployment_action 1.2.4 1.2.3 hmstream)" "replaced legacy hmstream" \
  "legacy replacement action"

echo "deploy_test: PASS"
