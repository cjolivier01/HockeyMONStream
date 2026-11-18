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
expect_equal "$(classify_target ubuntu 24.04 aarch64 jetson)" "jetson-ubuntu24.04-arm64" \
  "Jetson classification"
if classify_target ubuntu 22.04 aarch64 jetson >/dev/null; then
  fail "obsolete Jetson baseline was accepted"
fi
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

  # The resolver globs, but nullglob belongs to whoever sourced the library.
  for nullglob_state in u s; do
    shopt -"${nullglob_state}" nullglob
    TOPDIR="${fixture_topdir}" HOME="${fixture_home}" HSTREAM_DEEPSTREAM_CACHE='' \
      deepstream_artifact_resolve test '' hint "${fixture_topdir}/dist" >/dev/null
    if shopt -q nullglob && [[ "${nullglob_state}" == u ]]; then
      fail "resolver left nullglob set"
    fi
    if ! shopt -q nullglob && [[ "${nullglob_state}" == s ]]; then
      fail "resolver cleared the caller's nullglob"
    fi
  done
  shopt -u nullglob

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

# Exercise the deployment boundary with real Debian metadata, without building
# on or installing anything to a remote node.
if command -v dpkg-deb >/dev/null; then
  fixture_root="$(mktemp -d)"
  trap 'rm -rf "${fixture_root}"' EXIT
  fixture_topdir="${fixture_root}/repo"
  mkdir -p "${fixture_topdir}/scripts"
  cat > "${fixture_topdir}/scripts/make_deb_jetson.sh" <<'BUILDER'
#!/usr/bin/env bash
set -euo pipefail
for argument in "$@"; do
  case "${argument}" in
    --host=*) [[ "${argument#*=}" == stubby ]] ;;
    --output-dir=*) output="${argument#*=}" ;;
    --version=*) version="${argument#*=}" ;;
    *) exit 1 ;;
  esac
done
mkdir -p "${output}/fixture/DEBIAN"
printf '%s\n' "${JETSON_QT}" > "${output}/requested-qt"
cat > "${output}/fixture/DEBIAN/control" <<CONTROL
Package: hstream
Version: ${version}
Architecture: arm64
Maintainer: test <test@example.invalid>
Description: Jetson deployment fixture
X-HStream-Target-Ubuntu: 24.04
X-HStream-Target-Platform: jetson
X-HStream-Qt: ${TEST_PACKAGE_QT:-${JETSON_QT}}
CONTROL
dpkg-deb --build --root-owner-group "${output}/fixture" "${output}/hstream_${version}_arm64.deb" >/dev/null
BUILDER
  chmod +x "${fixture_topdir}/scripts/make_deb_jetson.sh"
  PACKAGE_VERSION_NORMALIZED=1.2.3
  TARGET_BUILD_HOST[jetson-ubuntu24.04-arm64]=stubby
  for mode in 0 1; do
    TOPDIR="${fixture_topdir}" DEPLOY_OUTPUT_DIR="${fixture_root}/dist" JETSON_QT="${mode}" \
      build_target jetson-ubuntu24.04-arm64 >/dev/null || fail "Jetson Qt=${mode} package rejected"
    expect_equal "$(cat "${fixture_root}/dist/jetson/requested-qt")" "${mode}" \
      "deployment forwards Qt=${mode} to the Jetson builder"
    if TOPDIR="${fixture_topdir}" DEPLOY_OUTPUT_DIR="${fixture_root}/dist" JETSON_QT="${mode}" \
      TEST_PACKAGE_QT="$((1 - mode))" build_target jetson-ubuntu24.04-arm64 >/dev/null 2>&1; then
      fail "deployment accepted the wrong Qt package variant for Qt=${mode}"
    fi
  done
  if TOPDIR="${fixture_topdir}" DEPLOY_OUTPUT_DIR="${fixture_root}/dist" JETSON_QT=1 \
    TEST_PACKAGE_QT=unknown build_target jetson-ubuntu24.04-arm64 >/dev/null 2>&1; then
    fail "deployment accepted an unknown Qt package variant"
  fi
  rm -rf "${fixture_root}"
  trap - EXIT
fi

# A light deployment installs no DeepStream anywhere, so each node's own copy
# has to satisfy the package's dependency by itself.
expect_equal "$(deepstream_requirement_problem 'deepstream-9.1/9.1.0-1')" "" \
  "the minimum supported DeepStream satisfies a light deployment"
expect_equal "$(deepstream_requirement_problem 'deepstream-8.0/8.0.0-1 deepstream-9.1/9.1.1-1')" "" \
  "a supported DeepStream alongside an older one satisfies a light deployment"
expect_equal "$(deepstream_requirement_problem '')" \
  "deepstream-9.1 (>= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION}) is not installed" \
  "a node without DeepStream is reported"
expect_equal "$(deepstream_requirement_problem 'deepstream-9.2/9.2.0-1')" \
  "deepstream-9.1 (>= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION}) is not installed (found deepstream-9.2 9.2.0-1)" \
  "a node carrying only a newer DeepStream names what it has"
expect_equal "$(deepstream_requirement_problem 'deepstream-9.1/9.0.9-1')" \
  "deepstream-9.1 (>= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION}) is not installed (found deepstream-9.1 9.0.9-1)" \
  "an out-of-range DeepStream does not satisfy a light deployment"

# That check has to accept exactly what the package declares it needs.
# make_deb.sh builds the Depends field from its own copy of the name and the
# bounds, so run those literals through the check instead of trusting the two
# copies to stay equal.
builder_declares() {
  sed -nE "s/^ *$1=\"\\\$\{$1:-([^}]*)\}\"\$/\1/p" "${SCRIPT_DIR}/make_deb.sh" | sort -u
}
builder_package="$(builder_declares DEEPSTREAM_PACKAGE)"
builder_min="$(builder_declares DEEPSTREAM_MIN_VERSION)"
builder_max="$(builder_declares DEEPSTREAM_MAX_VERSION)"
for builder_value in "${builder_package}" "${builder_min}" "${builder_max}"; do
  if [[ -z "${builder_value}" || "$(printf '%s\n' "${builder_value}" | wc -l)" -ne 1 ]]; then
    fail "make_deb.sh does not declare one DeepStream dependency for the node check to match"
  fi
done
expect_equal "$(deepstream_requirement_problem "${builder_package}/${builder_min}")" "" \
  "the node check accepts the oldest DeepStream the package depends on"
if [[ -z "$(deepstream_requirement_problem "${builder_package}/${builder_max}")" ]]; then
  fail "the node check accepts a DeepStream version the package excludes"
fi
# Accepting the declared bounds is not enough on its own: a builder that raised
# its minimum would still be accepted here while the node's apt refused the
# package.  The two bounds have to be the same text, in all three copies.
expect_equal "${builder_min}" "${DEEPSTREAM_MIN_VERSION}" \
  "make_deb.sh and the deployment require the same minimum DeepStream"
expect_equal "${builder_max}" "${DEEPSTREAM_MAX_VERSION}" \
  "make_deb.sh and the deployment exclude the same DeepStream versions"
for bound in MIN MAX; do
  expect_equal \
    "$(sed -nE "s/^DEEPSTREAM_${bound}_VERSION=\"([^\"]*)\"\$/\1/p" "${SCRIPT_DIR}/install_deb.sh")" \
    "$(eval "printf '%s' \"\${DEEPSTREAM_${bound}_VERSION}\"")" \
    "install_deb.sh and the deployment agree on the ${bound} DeepStream version"
done

# Detection against a stubbed node: the response carries the DeepStream
# inventory, and a light deployment judges it before anything is built.
detect_with() {
  # Not "platform": detect_node declares a local of that name, and the stub
  # runs inside it, so the response would read back detect_node's own empty
  # variable instead of what this call asked for.
  local node="$1"
  local stub_platform="$2"
  local stub_deepstream="$3"
  ssh() {
    printf '__HSTREAM_DEPLOY__|ubuntu|24.04|debian|noble|%s|%s|hstream|1.2.2|%s\n' \
      "$([[ "${stub_platform}" == jetson ]] && printf aarch64 || printf x86_64)" \
      "${stub_platform}" "${stub_deepstream}"
  }
  NODE_RESULT=()
  NODE_DETAIL=()
  NODE_TARGET=()
  SEEN_TARGETS=()
  TARGET_KEYS=()
  detect_node "${node}" >/dev/null 2>&1
  local status=$?
  unset -f ssh
  return "${status}"
}

DEPLOY_DEEPSTREAM=0
if detect_with ripper desktop deepstream-9.2/9.2.0-1; then
  fail "a light deployment accepted a desktop without a usable DeepStream"
fi
expect_equal "${NODE_DETAIL[ripper]}" \
  "deepstream-9.1 (>= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION}) is not installed (found deepstream-9.2 9.2.0-1)" \
  "the rejected node reports what its DeepStream is"
expect_equal "${#TARGET_KEYS[@]}" 0 "a rejected node does not schedule a package build"
detect_with monster desktop deepstream-9.1/9.1.1-1 \
  || fail "a light deployment rejected a desktop with a usable DeepStream"
expect_equal "${NODE_TARGET[monster]}" desktop-ubuntu24.04-amd64 "accepted light desktop target"

# A Jetson never receives a DeepStream artifact, but its plain apt-get install
# would pull a missing one from the node's own repositories, so it is judged
# too.  A rejected node must not become the build host for its target either.
if detect_with stubby jetson ''; then
  fail "a light deployment accepted a Jetson whose apt would install DeepStream"
fi
expect_equal "${#TARGET_KEYS[@]}" 0 "a rejected Jetson does not become a build host"
detect_with stubby jetson deepstream-9.1/9.1.1-1 \
  || fail "a light deployment rejected a Jetson with a usable DeepStream"

DEPLOY_DEEPSTREAM=1
detect_with stubby jetson '' \
  || fail "an ordinary deployment stopped deploying to a Jetson without DeepStream"
detect_with ripper desktop deepstream-9.2/9.2.0-1 \
  || fail "an ordinary deployment stopped installing DeepStream over an unsupported version"

if JETSON_QT=invalid require_local_commands >/dev/null 2>&1; then
  fail "deployment accepted an invalid JETSON_QT option"
fi
if DEPLOY_DEEPSTREAM=invalid require_local_commands >/dev/null 2>&1; then
  fail "deployment accepted an invalid DEPLOY_DEEPSTREAM option"
fi
if output="$(DEPLOY_DEEPSTREAM=2 NODES=stubby bash "${SCRIPT_DIR}/deploy.sh" 2>&1)"; then
  fail "deploy.sh accepted an invalid DEPLOY_DEEPSTREAM option"
fi
expect_equal "${output}" 'ERROR: DEPLOY_DEEPSTREAM must be 0 or 1.' \
  "deploy.sh rejects an invalid DeepStream option before repository, SSH or package operations"

# What a desktop node actually receives.  Record the upload and the remote
# command instead of reading the branch that builds them: the DeepStream
# artifact not being uploaded is the whole point of a light deployment.
desktop_argv_dir="$(mktemp -d)"
trap 'rm -rf "${desktop_argv_dir}"' EXIT
mkdir -p "${desktop_argv_dir}/bin"
cat >"${desktop_argv_dir}/bin/sudo" <<SUDO_STUB
#!/usr/bin/env bash
printf '%s\n' "\$@" >"${desktop_argv_dir}/installer"
SUDO_STUB
printf '#!/usr/bin/env bash\nexit 0\n' >"${desktop_argv_dir}/bin/chmod"
printf '#!/usr/bin/env bash\nprintf node\\n\n' >"${desktop_argv_dir}/bin/hostname"
chmod +x "${desktop_argv_dir}/bin/sudo" "${desktop_argv_dir}/bin/chmod" \
  "${desktop_argv_dir}/bin/hostname"

record_desktop_deployment() {
  (
    create_remote_directory() { printf '/tmp/hstream-deploy.testdir'; }
    cleanup_remote_directory() { :; }
    scp() { printf '%s\n' "$@" >"${desktop_argv_dir}/scp"; }
    # ssh does not deliver an argument vector: it joins its arguments into one
    # string that the remote shell parses again.  Reproduce that instead of
    # recording the local list, so the test sees what the installer is really
    # handed -- an empty argument disappearing is invisible otherwise.
    ssh() {
      printf '%s\n' "$@" >"${desktop_argv_dir}/ssh"
      while [[ "$1" == -o ]]; do shift 2; done
      shift
      PATH="${desktop_argv_dir}/bin:${PATH}" bash -c "$*"
    }
    DEPLOY_DEEPSTREAM="$1" \
      DESKTOP_DEEPSTREAM_DEB=/artifacts/deepstream-9.1_9.1.1-1_amd64.deb \
      deploy_desktop monster /dist/hstream_1.2.3_amd64.deb /repo/scripts/install_deb.sh >/dev/null
  ) || fail "deploy_desktop failed with DEPLOY_DEEPSTREAM=$1"
}

expect_installer_argv() {
  local label="$1"
  shift
  local expected
  expected="$(printf '%s\n' "$@")"
  expect_equal "$(cat "${desktop_argv_dir}/installer")" "${expected}" "${label}"
}

record_desktop_deployment 0
if grep -q deepstream "${desktop_argv_dir}/scp"; then
  fail "a light deployment uploaded a DeepStream artifact"
fi
expect_equal "$(grep -c '^/' "${desktop_argv_dir}/scp")" 2 \
  "a light deployment uploads only the package and the installer"
expect_installer_argv "a light deployment runs the installer with --no-deepstream" \
  -n /tmp/hstream-deploy.testdir/install_deb.sh \
  --force-hstream \
  --no-deepstream \
  --hstream-deb=/tmp/hstream-deploy.testdir/hstream_1.2.3_amd64.deb

record_desktop_deployment 1
if ! grep -qx /artifacts/deepstream-9.1_9.1.1-1_amd64.deb "${desktop_argv_dir}/scp"; then
  fail "an ordinary deployment stopped uploading the DeepStream artifact"
fi
expect_installer_argv "an ordinary deployment installs the uploaded DeepStream artifact" \
  -n /tmp/hstream-deploy.testdir/install_deb.sh \
  --force-hstream \
  --deepstream-deb=/tmp/hstream-deploy.testdir/deepstream-9.1_9.1.1-1_amd64.deb \
  --hstream-deb=/tmp/hstream-deploy.testdir/hstream_1.2.3_amd64.deb
rm -rf "${desktop_argv_dir}"
trap - EXIT

# The installer is copied to each node on its own and carries no copy of this
# file, so pin the option deploy.sh passes it to the one it parses.
if ! grep -q -- '--no-deepstream)' "${SCRIPT_DIR}/install_deb.sh"; then
  fail "install_deb.sh does not accept the --no-deepstream option deploy.sh passes it"
fi
installer_rejection="$(bash "${SCRIPT_DIR}/install_deb.sh" --no-deepstream \
  --deepstream-deb=/dev/null --hstream-deb=/dev/null 2>&1 | head -n 1 || true)"
expect_equal "${installer_rejection}" \
  'ERROR: --no-deepstream and --deepstream-deb request opposite things.' \
  "the installer refuses to both keep and replace DeepStream"
installer_rejection="$(bash "${SCRIPT_DIR}/install_deb.sh" --hstream-deb=/dev/null 2>&1 | head -n 1 || true)"
expect_equal "${installer_rejection}" \
  'ERROR: --hstream-deb and --deepstream-deb are required.' \
  "the installer still requires a DeepStream artifact by default"

for script in deploy.sh make_deb.sh make_deb_jetson.sh; do
  if output="$(JETSON_QT=invalid NODES=stubby bash "${SCRIPT_DIR}/${script}" 2>&1)"; then
    fail "${script} accepted an invalid JETSON_QT option"
  fi
  expect_equal "${output}" 'ERROR: JETSON_QT must be 0 or 1.' \
    "${script} rejects invalid Qt options before repository, SSH or package operations"
done

echo "deploy_test: PASS"
