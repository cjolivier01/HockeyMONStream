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
  # A fixture carries /opt/nvidia/deepstream unless asked not to: that tree is
  # what separates a DeepStream release from a metapackage wearing its name.
  make_deepstream_fixture() {
    local path="$1" package="$2" version="$3" architecture="$4" content="${5:-runtime}"
    local root="${fixture_root}/build/$(basename "${path}")"
    mkdir -p "${root}/DEBIAN" "$(dirname "${path}")"
    if [[ "${content}" == runtime ]]; then
      mkdir -p "${root}/opt/nvidia/deepstream/deepstream/lib"
      printf 'fixture\n' >"${root}/opt/nvidia/deepstream/deepstream/lib/libnvds.so"
    else
      mkdir -p "${root}/usr/share/doc/${package}"
      printf 'fixture\n' >"${root}/usr/share/doc/${package}/README"
    fi
    cat >"${root}/DEBIAN/control" <<CONTROL
Package: ${package}
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
  make_deepstream_fixture "${fixture_home}/Downloads/deepstream-9.1_9.1.0-1_amd64.deb" \
    deepstream-9.1 9.1.0-1 amd64
  make_deepstream_fixture "${fixture_sibling}/deepstream-9.1_9.1.1-1_amd64.deb" \
    deepstream-9.1 9.1.1-1 amd64
  make_deepstream_fixture "${fixture_home}/deepstream-9.1_9.1.9-1_amd64.deb" \
    deepstream-9.1 9.1.9-1 arm64
  make_deepstream_fixture "${fixture_home}/deepstream-9.1_9.2.0-1_amd64.deb" \
    deepstream-9.1 9.2.0-1 amd64

  # Not $(...): the resolver reports through a global, which a subshell loses.
  TOPDIR="${fixture_topdir}" HOME="${fixture_home}" HSTREAM_DEEPSTREAM_CACHE='' \
    deepstream_artifact_resolve test '' hint "${fixture_topdir}/dist" \
    >"${fixture_root}/resolve.log" || fail "resolver rejected a usable artifact"
  resolve_output="$(cat "${fixture_root}/resolve.log")"
  expect_equal "${DEEPSTREAM_ARTIFACT}" "${fixture_sibling}/deepstream-9.1_9.1.1-1_amd64.deb" \
    "newest supported amd64 artifact wins"
  expect_equal "${DEEPSTREAM_ARTIFACT_PACKAGE}" deepstream-9.1 "resolver reports the release it picked"
  expect_equal "${DEEPSTREAM_ARTIFACT_VERSION}" 9.1.1-1 "resolver reports the version it picked"
  expect_equal "${resolve_output}" "[test] Using deepstream-9.1 9.1.1-1 from ${DEEPSTREAM_ARTIFACT}" \
    "resolver reports what it picked"

  if TOPDIR="${fixture_root}/bare/repo" HOME="${fixture_root}/bare/home" \
    HSTREAM_DEEPSTREAM_CACHE='' deepstream_artifact_resolve test '' hint \
    "${fixture_root}/bare/repo/dist" >/dev/null 2>&1; then
    fail "resolver accepted a tree with no usable artifact"
  fi
  # deepstream-9.1 carrying 9.2.0-1 cannot satisfy the dependency a package
  # built against deepstream-9.1 declares, so it is not a usable artifact.
  if TOPDIR="${fixture_topdir}" deepstream_artifact_resolve test \
    "${fixture_home}/deepstream-9.1_9.2.0-1_amd64.deb" hint >/dev/null 2>&1; then
    fail "resolver accepted a version outside the release its name claims"
  fi

  # A newer release outranks a newer version of an older one, and the release
  # is read from the package name rather than the file name.
  make_deepstream_fixture "${fixture_home}/Downloads/deepstream-9.2_9.2.0-1_amd64.deb" \
    deepstream-9.2 9.2.0-1 amd64
  TOPDIR="${fixture_topdir}" HOME="${fixture_home}" HSTREAM_DEEPSTREAM_CACHE='' \
    deepstream_artifact_resolve test '' hint "${fixture_topdir}/dist" >/dev/null \
    || fail "resolver rejected a newer DeepStream release"
  expect_equal "${DEEPSTREAM_ARTIFACT_PACKAGE}" deepstream-9.2 "a newer release outranks an older one"

  # A compatibility metapackage declares the release and ships none of it.
  # The newest usable artifact wins, which is not the newest artifact.
  make_deepstream_fixture "${fixture_home}/Downloads/deepstream-9.3_9.3.0-1_amd64.deb" \
    deepstream-9.3 9.3.0-1 amd64 metapackage
  TOPDIR="${fixture_topdir}" HOME="${fixture_home}" HSTREAM_DEEPSTREAM_CACHE='' \
    deepstream_artifact_resolve test '' hint "${fixture_topdir}/dist" \
    >/dev/null 2>"${fixture_root}/resolve-passed-over.log" \
    || fail "resolver gave up instead of passing over a metapackage"
  expect_equal "${DEEPSTREAM_ARTIFACT_PACKAGE}" deepstream-9.2 \
    "a release that ships no DeepStream is passed over for one that does"
  grep -qF 'deepstream-9.3 9.3.0-1' "${fixture_root}/resolve-passed-over.log" \
    || fail "the passed-over metapackage was not named"
  if TOPDIR="${fixture_topdir}" deepstream_artifact_resolve test \
    "${fixture_home}/Downloads/deepstream-9.3_9.3.0-1_amd64.deb" hint >/dev/null 2>&1; then
    fail "resolver accepted a metapackage named explicitly"
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
# has to satisfy the dependency its package will declare.  A desktop node is
# judged against the release the resolved artifact carries; a Jetson builds
# against its own SDK, so any supported release satisfies it.
expect_equal "$(deepstream_requirement_problem 'deepstream-9.1/9.1.0-1' deepstream-9.1)" "" \
  "the oldest version of the required release satisfies a light deployment"
expect_equal "$(deepstream_requirement_problem 'deepstream-8.0/8.0.0-1 deepstream-9.1/9.1.1-1' deepstream-9.1)" "" \
  "the required release alongside an older one satisfies a light deployment"
expect_equal "$(deepstream_requirement_problem '' deepstream-9.1)" \
  "deepstream-9.1 (>= 9.1.0-1, << 9.2~) is not installed" \
  "a node without DeepStream is reported"
expect_equal "$(deepstream_requirement_problem 'deepstream-9.2/9.2.0-1' deepstream-9.1)" \
  "deepstream-9.1 (>= 9.1.0-1, << 9.2~) is not installed (found deepstream-9.2 9.2.0-1)" \
  "a node carrying a different release names what it has"
expect_equal "$(deepstream_requirement_problem 'deepstream-9.1/9.0.9-1' deepstream-9.1)" \
  "deepstream-9.1 (>= 9.1.0-1, << 9.2~) is not installed (found deepstream-9.1 9.0.9-1)" \
  "a version below the required release does not satisfy a light deployment"

# Building against a newer release moves the requirement with it: the same
# node that satisfied 9.1 does not satisfy a package built against 9.2.
expect_equal "$(deepstream_requirement_problem 'deepstream-9.2/9.2.0-1' deepstream-9.2)" "" \
  "a node running the newer release satisfies a package built against it"
expect_equal "$(deepstream_requirement_problem 'deepstream-9.1/9.1.1-1' deepstream-9.2)" \
  "deepstream-9.2 (>= 9.2.0-1, << 9.3~) is not installed (found deepstream-9.1 9.1.1-1)" \
  "a package built against the newer release is not satisfied by the older one"

# A Jetson package is built on the node against whatever it has, so the check
# asks only that it has something supported.
expect_equal "$(deepstream_requirement_problem 'deepstream-9.2/9.2.0-1' '')" "" \
  "any supported release satisfies a node that builds its own package"
expect_equal "$(deepstream_requirement_problem 'deepstream-8.0/8.0.0-1' '')" \
  "no DeepStream >= ${DEEPSTREAM_MIN_VERSION} is installed (found deepstream-8.0 8.0.0-1)" \
  "a release older than the floor satisfies nothing"
expect_equal "$(deepstream_requirement_problem 'deepstream-sample-data/9.1.0' '')" \
  "no DeepStream >= ${DEEPSTREAM_MIN_VERSION} is installed (found deepstream-sample-data 9.1.0)" \
  "a deepstream package that is not a release does not count as one"

# make_deb.sh writes the Depends from the release it built against, and
# install_deb.sh reads the requirement back out of it.  Neither carries a
# release of its own any more, so run the writer's output through the reader
# instead of pinning two copies of a constant.  The reader is loaded in a
# subshell: it is a production installer, and sourcing it here would put its
# variables in this one.
read_requirement_back() {
  HSTREAM_INSTALLER_SOURCE_ONLY=1 bash -c '
    # shellcheck disable=SC1090
    source "$1"
    read_deepstream_requirement "$2" || exit 1
    printf "%s %s %s" "${DEEPSTREAM_PACKAGE}" "${DEEPSTREAM_MIN_VERSION}" "${DEEPSTREAM_MAX_VERSION}"
  ' _ "${SCRIPT_DIR}/install_deb.sh" "$1"
}

for pinned_release in deepstream-9.1 deepstream-9.2 deepstream-10.3; do
  read -r pinned_name pinned_min pinned_max < <(deepstream_dependency_bounds "${pinned_release}")
  written="${pinned_name} (>= ${pinned_min}), ${pinned_name} (<< ${pinned_max})"
  expect_equal "$(read_requirement_back "libc6 (>= 2.38), ${written}, hugin-tools")" \
    "${pinned_name} ${pinned_min} ${pinned_max}" \
    "install_deb.sh reads back the dependency make_deb.sh writes for ${pinned_release}"
done

# The reader has to refuse what it cannot act on rather than guess.
for refusable in \
  "libc6 (>= 2.38)" \
  "deepstream-9.1 (>= 9.1.0-1)" \
  "deepstream-9.1 (>= 9.1.0-1), deepstream-9.2 (<< 9.3~)" \
  "deepstream-sample-data (>= 9.1.0), deepstream-sample-data (<< 9.2)"; do
  if read_requirement_back "${refusable}" >/dev/null 2>&1; then
    fail "install_deb.sh accepted a DeepStream dependency it cannot act on: ${refusable}"
  fi
done
# A package whose name merely contains one must not be read as the real
# relation, and the real one beside it must still be found.
expect_equal "$(read_requirement_back "libdeepstream-9.1 (>= 1), deepstream-9.2 (>= 9.2.0-1), deepstream-9.2 (<< 9.3~)")" \
  "deepstream-9.2 9.2.0-1 9.3~" "a lookalike package name is not read as the DeepStream dependency"

# The bounds this repository shipped when 9.1 was the only release it knew.
# Deriving them has to keep producing them, or every already-installed node
# stops matching a package rebuilt from this tree.
expect_equal "$(deepstream_dependency_bounds deepstream-9.1)" "deepstream-9.1 9.1.0-1 9.2~" \
  "the derived 9.1 dependency is the one this repository has always shipped"

# Detection against a stubbed node: the response carries the node's DeepStream
# inventory, which the light-deployment verdict reads afterwards.
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
  detect_node "${node}" >/dev/null 2>&1
  local status=$?
  unset -f ssh
  return "${status}"
}

reset_node_state() {
  NODES_LIST=()
  NODE_RESULT=()
  NODE_DETAIL=()
  NODE_TARGET=()
  NODE_DEEPSTREAM=()
  SEEN_TARGETS=()
  TARGET_KEYS=()
  TARGET_BUILD_HOST=()
}

# Detection records the inventory and classifies the node; it no longer
# decides anything about DeepStream, because the release a desktop package
# will ask for is only known once the artifact has been resolved.
reset_node_state
NODES_LIST=(ripper)
detect_with ripper desktop deepstream-9.2/9.2.0-1 || fail "detection rejected a reachable node"
expect_equal "${NODE_DEEPSTREAM[ripper]}" "deepstream-9.2/9.2.0-1" "detection records the node inventory"
expect_equal "${NODE_TARGET[ripper]}" desktop-ubuntu24.04-amd64 "detection classifies the node"
expect_equal "${#TARGET_KEYS[@]}" 1 "detection schedules the node's target"

# A desktop node is judged against the release the desktop artifact carries.
if judge_node_deepstream ripper deepstream-9.1 >/dev/null 2>&1; then
  fail "a light deployment accepted a desktop without the release its package needs"
fi
expect_equal "${NODE_DETAIL[ripper]}" \
  "deepstream-9.1 (>= 9.1.0-1, << 9.2~) is not installed (found deepstream-9.2 9.2.0-1)" \
  "the rejected node reports what its DeepStream is"
rebuild_target_set
expect_equal "${#TARGET_KEYS[@]}" 0 "a rejected node does not schedule a package build"

# The same node satisfies a package built against the release it does have.
reset_node_state
NODES_LIST=(ripper)
detect_with ripper desktop deepstream-9.2/9.2.0-1 || fail "detection rejected a reachable node"
judge_node_deepstream ripper deepstream-9.2 \
  || fail "a light deployment rejected a desktop running the release its package needs"
rebuild_target_set
expect_equal "${#TARGET_KEYS[@]}" 1 "an accepted node keeps its target"
expect_equal "${TARGET_BUILD_HOST[desktop-ubuntu24.04-amd64]}" ripper "an accepted node can build its target"

# A Jetson never receives an artifact and builds against its own SDK, so it is
# judged against the floor rather than a named release -- but it is judged,
# because its plain apt-get install would otherwise pull a missing DeepStream
# from the node's own repositories.
reset_node_state
NODES_LIST=(stubby)
detect_with stubby jetson '' || fail "detection rejected a reachable Jetson"
if judge_node_deepstream stubby "" >/dev/null 2>&1; then
  fail "a light deployment accepted a Jetson whose apt would install DeepStream"
fi
rebuild_target_set
expect_equal "${#TARGET_KEYS[@]}" 0 "a rejected Jetson does not become a build host"

reset_node_state
NODES_LIST=(stubby)
detect_with stubby jetson deepstream-9.2/9.2.0-1 || fail "detection rejected a reachable Jetson"
judge_node_deepstream stubby "" \
  || fail "a light deployment rejected a Jetson carrying a supported release"

# Only the first node of a target builds it, and a rejected first node hands
# that job to a surviving one rather than taking the target down with it.
reset_node_state
NODES_LIST=(ripper monster)
detect_with ripper desktop deepstream-9.1/9.1.1-1 || fail "detection rejected a reachable node"
detect_with monster desktop deepstream-9.2/9.2.0-1 || fail "detection rejected a reachable node"
expect_equal "${TARGET_BUILD_HOST[desktop-ubuntu24.04-amd64]}" ripper "the first node builds its target"
judge_node_deepstream ripper deepstream-9.2 >/dev/null 2>&1 \
  && fail "a light deployment accepted a desktop without the release its package needs"
judge_node_deepstream monster deepstream-9.2 || fail "the surviving node was rejected"
rebuild_target_set
expect_equal "${TARGET_BUILD_HOST[desktop-ubuntu24.04-amd64]}" monster \
  "a rejected build host is replaced by a node still in play"

reset_node_state
NODES_LIST=(stubby)
DEPLOY_DEEPSTREAM=1
detect_with stubby jetson '' \
  || fail "an ordinary deployment stopped deploying to a Jetson without DeepStream"
detect_with ripper desktop deepstream-9.2/9.2.0-1 \
  || fail "an ordinary deployment stopped installing DeepStream over another release"
reset_node_state

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
args=("\$@")
if [[ "\${args[0]:-}" == -n ]]; then args=("\${args[@]:1}"); fi
if [[ "\${args[0]:-}" == apt-get ]]; then exec "\${args[@]}"; fi
printf '%s\n' "\$@" >"${desktop_argv_dir}/installer"
SUDO_STUB
# Answers a simulation with the plan the case under test wants APT to return,
# and records the installations it would really have performed.
cat >"${desktop_argv_dir}/bin/apt-get" <<APT_STUB
#!/usr/bin/env bash
for argument in "\$@"; do
  if [[ "\${argument}" == --simulate ]]; then
    printf '%s\n' "\${HSTREAM_TEST_APT_PLAN:-}"
    exit "\${HSTREAM_TEST_APT_SIMULATE_STATUS:-0}"
  fi
done
printf '%s\n' "\$*" >>"${desktop_argv_dir}/apt-ran"
APT_STUB
printf '#!/usr/bin/env bash\nexit 0\n' >"${desktop_argv_dir}/bin/chmod"
printf '#!/usr/bin/env bash\nprintf node\\n\n' >"${desktop_argv_dir}/bin/hostname"
chmod +x "${desktop_argv_dir}/bin/sudo" "${desktop_argv_dir}/bin/apt-get" \
  "${desktop_argv_dir}/bin/chmod" "${desktop_argv_dir}/bin/hostname"

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

# A Jetson receives no DeepStream artifact and runs apt-get directly, so the
# promise has to be enforced against the plan apt-get is about to carry out.
# The node passed its inventory check before the build; this is the window
# between that check and the installation.
run_jetson_deployment() {
  # Named, not positional: the stub runs inside deploy_jetson, where $1 and $2
  # are its arguments rather than this function's.
  local stub_mode="$1"
  local stub_apt_plan="$2"
  local stub_apt_status="${3:-0}"
  rm -f "${desktop_argv_dir}/apt-ran"
  (
    create_remote_directory() { printf '/tmp/hstream-deploy.testdir'; }
    cleanup_remote_directory() { :; }
    scp() { :; }
    ssh() {
      while [[ "$1" == -o ]]; do shift 2; done
      shift
      HSTREAM_TEST_APT_PLAN="${stub_apt_plan}" \
        HSTREAM_TEST_APT_SIMULATE_STATUS="${stub_apt_status}" \
        PATH="${desktop_argv_dir}/bin:${PATH}" \
        bash -c "$*"
    }
    DEPLOY_DEEPSTREAM="${stub_mode}" deploy_jetson stubby /dist/hstream_1.2.3_arm64.deb
  ) >"${desktop_argv_dir}/jetson" 2>&1
}

jetson_clean_plan='Inst hstream (1.2.3 local [arm64])
Conf hstream (1.2.3 local [arm64])'
jetson_deepstream_plan='Inst deepstream-9.1 [9.1.0-1] (9.1.1-1 NVIDIA:noble [arm64])
Inst hstream (1.2.3 local [arm64])'

if run_jetson_deployment 0 "${jetson_deepstream_plan}"; then
  fail "a light Jetson deployment installed over a plan that changes DeepStream"
fi
if [[ -f "${desktop_argv_dir}/apt-ran" ]]; then
  fail "a light Jetson deployment ran apt after refusing the plan"
fi
if ! grep -q 'Inst deepstream-9.1' "${desktop_argv_dir}/jetson"; then
  fail "the refused Jetson deployment did not report the DeepStream change it found"
fi

run_jetson_deployment 0 "${jetson_clean_plan}" \
  || fail "a light Jetson deployment refused a plan that leaves DeepStream alone"
if ! grep -q 'hstream_1.2.3_arm64.deb' "${desktop_argv_dir}/apt-ran"; then
  fail "a light Jetson deployment did not install the package"
fi

run_jetson_deployment 1 "${jetson_deepstream_plan}" \
  || fail "an ordinary Jetson deployment started refusing DeepStream changes"
if ! grep -q 'hstream_1.2.3_arm64.deb' "${desktop_argv_dir}/apt-ran"; then
  fail "an ordinary Jetson deployment did not install the package"
fi

# An unreadable plan is not an empty plan.  The remote script reads the
# simulation through a pipe, so without pipefail apt's failure would be hidden
# by the exit status of the filter and the installation would go ahead anyway.
if run_jetson_deployment 0 "${jetson_clean_plan}" 100; then
  fail "a light Jetson deployment installed after apt could not produce a plan"
fi
if [[ -f "${desktop_argv_dir}/apt-ran" ]]; then
  fail "a light Jetson deployment ran apt after its simulation failed"
fi
rm -rf "${desktop_argv_dir}"
trap - EXIT

# Four scripts read APT's plan for DeepStream changes, and two of them run on
# a remote node from a quoted here-document, so they cannot share one
# definition.  Require the filter to be the same text in all four instead.
# Requiring one occurrence per file is part of the pin, not tidiness: the
# extraction below takes the longest match on a line, so a second copy left in
# a comment could stand in for a filter that had actually been changed.
plan_filter_text() {
  local occurrences
  occurrences="$(grep -o 'print \$1 " " \$2 }' "$1" | wc -l)"
  if [[ "${occurrences}" -ne 1 ]]; then
    fail "$(basename "$1") has ${occurrences} DeepStream plan filters; one is pinned, so one is allowed"
  fi
  sed -n 's/.*awk .\(($1 == "Inst".*print \$1 " " \$2 }\).*/\1/p' "$1"
}
installer_plan_filter="$(plan_filter_text "${SCRIPT_DIR}/install_deb.sh")"
[[ -n "${installer_plan_filter}" ]] || fail "no DeepStream plan filter found in install_deb.sh"
for peer in deploy.sh make_deb_jetson.sh make_deb.sh; do
  expect_equal "$(plan_filter_text "${SCRIPT_DIR}/${peer}")" "${installer_plan_filter}" \
    "${peer} reads an APT plan for DeepStream changes the same way install_deb.sh does"
done

# A Jetson package builds on the node, and make_deb.sh installs the packaging
# tools that host is missing, so the choice has to reach it as well.
if ! grep -q 'DEPLOY_DEEPSTREAM="${deploy_deepstream}"' "${SCRIPT_DIR}/make_deb_jetson.sh"; then
  fail "make_deb_jetson.sh does not pass the DeepStream choice to make_deb.sh"
fi
if ! grep -q 'DEPLOY_DEEPSTREAM="${DEPLOY_DEEPSTREAM:-1}"' "${SCRIPT_DIR}/make_deb.sh"; then
  fail "make_deb.sh does not accept the DeepStream choice make_deb_jetson.sh passes it"
fi
# The helper installs "${package}", so a literal package name on one of these
# lines is a tool being installed somewhere else.
unguarded_tool_installs="$(grep -nE '^[[:space:]]*sudo apt-get install -y [a-z]' "${SCRIPT_DIR}/make_deb.sh" || true)"
if [[ -n "${unguarded_tool_installs}" ]]; then
  fail "make_deb.sh installs a packaging tool outside the guarded helper: ${unguarded_tool_installs}"
fi
if output="$(DEPLOY_DEEPSTREAM=2 bash "${SCRIPT_DIR}/make_deb.sh" --version 1.2.3 2>&1)"; then
  fail "make_deb.sh accepted an invalid DEPLOY_DEEPSTREAM option"
fi
expect_equal "${output}" 'ERROR: DEPLOY_DEEPSTREAM must be 0 or 1.' \
  "make_deb.sh rejects an invalid DeepStream option before packaging"

# The Jetson build installs its own missing dependencies on the node, so the
# light-deployment choice has to reach the builder, not only the installation
# after it.
if ! grep -q 'DEPLOY_DEEPSTREAM="${DEPLOY_DEEPSTREAM}"' "${SCRIPT_DIR}/deploy.sh"; then
  fail "deploy.sh does not pass the DeepStream choice to the Jetson package builder"
fi
if ! grep -q 'DEPLOY_DEEPSTREAM="${DEPLOY_DEEPSTREAM:-1}"' "${SCRIPT_DIR}/make_deb_jetson.sh"; then
  fail "make_deb_jetson.sh does not accept the DeepStream choice deploy.sh passes it"
fi
if output="$(DEPLOY_DEEPSTREAM=2 bash "${SCRIPT_DIR}/make_deb_jetson.sh" --host=stubby 2>&1)"; then
  fail "make_deb_jetson.sh accepted an invalid DEPLOY_DEEPSTREAM option"
fi
expect_equal "${output}" 'ERROR: DEPLOY_DEEPSTREAM must be 0 or 1.' \
  "make_deb_jetson.sh rejects an invalid DeepStream option before building"

# The installer is copied to each node on its own and carries no copy of this
# file.  Running it is what pins the option deploy.sh passes to the one it
# parses: an installer that dropped --no-deepstream would reject it as unknown
# rather than answer with the message below.
installer_rejection="$(bash "${SCRIPT_DIR}/install_deb.sh" --no-deepstream \
  --deepstream-deb=/dev/null --hstream-deb=/dev/null 2>&1 | head -n 1 || true)"
expect_equal "${installer_rejection}" \
  'ERROR: --no-deepstream and --deepstream-deb request opposite things.' \
  "the installer refuses to both keep and replace DeepStream"
installer_rejection="$(bash "${SCRIPT_DIR}/install_deb.sh" --hstream-deb=/dev/null 2>&1 | head -n 1 || true)"
expect_equal "${installer_rejection}" \
  'ERROR: --hstream-deb and --deepstream-deb are required.' \
  "the installer still requires a DeepStream artifact by default"

# Installing deepstream-9.2 over deepstream-9.1 means taking paths dpkg still
# records as 9.1's, which it refuses without a Replaces.  NVIDIA's artifacts
# declare Conflicts and no Replaces at all, so build the transition copy the
# installer builds and read the fields back off it.
# zstd is what NVIDIA compresses these artifacts with, and the installer runs
# tar against the executable rather than dpkg-deb's own library.
if ! command -v dpkg-deb >/dev/null || ! command -v ar >/dev/null ||
   ! command -v zstd >/dev/null; then
  echo "deploy_test: SKIPPING DeepStream transition checks (dpkg-deb, ar or zstd unavailable)" >&2
else
  transition_root="$(mktemp -d)"
  trap 'rm -rf "${transition_root}"' EXIT

  # $1 is what the artifact declares before the rewrite, written verbatim so a
  # case can hand over a folded field or a trailing blank line; the payload
  # stands in for the gigabyte of DeepStream the copy carries through.
  make_transition_fixture() {
    local relationships="$1" path="$2"
    local root
    root="${transition_root}/build/$(basename "${path}" .deb)"
    mkdir -p "${root}/DEBIAN" "${root}/opt/nvidia/deepstream"
    head -c 4096 /dev/urandom >"${root}/opt/nvidia/deepstream/payload.bin"
    {
      printf 'Package: deepstream-9.2\nVersion: 9.2.0-1\nArchitecture: amd64\n'
      printf 'Maintainer: test <test@example.invalid>\n'
      printf 'Description: deploy_test fixture\n'
      [[ -n "${relationships}" ]] && printf '%s\n' "${relationships}"
    } >"${root}/DEBIAN/control"
    dpkg-deb -Zzstd --build --root-owner-group "${root}" "${path}" >/dev/null
  }

  # The installer is a production script, so run it in a subshell the way the
  # dependency reader above is run.
  build_transition() {
    HSTREAM_INSTALLER_SOURCE_ONLY=1 bash -c '
      # shellcheck disable=SC1090
      source "$1"
      shift
      build_deepstream_transition_deb "$@"
    ' _ "${SCRIPT_DIR}/install_deb.sh" "$@"
  }

  # Exactly what deepstream-9.2_9.2.0-1_amd64.deb declares: a Conflicts to
  # extend and no Replaces to extend.
  make_transition_fixture 'Conflicts: libglapi-amber, deepstream-binaries-x86' \
    "${transition_root}/nvidia.deb"
  build_transition "${transition_root}/nvidia.deb" "${transition_root}/nvidia-transition.deb" \
    deepstream-9.1 || fail "the installer could not build a transition artifact"
  expect_equal "$(dpkg-deb -f "${transition_root}/nvidia-transition.deb" Conflicts)" \
    "libglapi-amber, deepstream-binaries-x86, deepstream-9.1" \
    "the transition copy keeps the artifact's own conflicts and adds the old release"
  expect_equal "$(dpkg-deb -f "${transition_root}/nvidia-transition.deb" Replaces)" \
    "deepstream-9.1" "the transition copy declares a Replaces the artifact does not have"
  expect_equal "$(dpkg-deb -f "${transition_root}/nvidia-transition.deb" Version)" 9.2.0-1 \
    "the transition copy is still the artifact it was made from"
  mkdir "${transition_root}/unpacked"
  dpkg-deb -x "${transition_root}/nvidia-transition.deb" "${transition_root}/unpacked" \
    || fail "the transition artifact does not unpack"
  cmp -s "${transition_root}/unpacked/opt/nvidia/deepstream/payload.bin" \
    "${transition_root}/build/nvidia/opt/nvidia/deepstream/payload.bin" \
    || fail "the transition artifact did not carry its data member through unchanged"
  # Contents alone would still pass if the member had been recompressed, which
  # on the real artifact means a gigabyte of work the copy must not do.
  cmp -s <(ar p "${transition_root}/nvidia.deb" "$(ar t "${transition_root}/nvidia.deb" \
    | grep '^data[.]tar')") \
    <(ar p "${transition_root}/nvidia-transition.deb" "$(ar t "${transition_root}/nvidia-transition.deb" \
    | grep '^data[.]tar')") \
    || fail "the transition artifact re-encoded its data member instead of copying it"

  # An existing field is extended rather than replaced, and more than one
  # installed release is named in both fields.
  make_transition_fixture \
    'Conflicts: libglapi-amber
Replaces: deepstream-sample-data' "${transition_root}/both.deb"
  build_transition "${transition_root}/both.deb" "${transition_root}/both-transition.deb" \
    deepstream-9.1 deepstream-8.0 || fail "the installer could not extend existing relationships"
  expect_equal "$(dpkg-deb -f "${transition_root}/both-transition.deb" Replaces)" \
    "deepstream-sample-data, deepstream-9.1, deepstream-8.0" \
    "an existing Replaces is extended with every installed release"
  expect_equal "$(dpkg-deb -f "${transition_root}/both-transition.deb" Conflicts)" \
    "libglapi-amber, deepstream-9.1, deepstream-8.0" \
    "an existing Conflicts is extended with every installed release"

  # Neither field present: an artifact that declares nothing still gets both.
  make_transition_fixture '' "${transition_root}/bare.deb"
  build_transition "${transition_root}/bare.deb" "${transition_root}/bare-transition.deb" \
    deepstream-9.1 || fail "the installer could not add both relationships"
  expect_equal "$(dpkg-deb -f "${transition_root}/bare-transition.deb" Conflicts)" \
    "deepstream-9.1" "a Conflicts is added to an artifact that declares none"
  expect_equal "$(dpkg-deb -f "${transition_root}/bare-transition.deb" Replaces)" \
    "deepstream-9.1" "a Replaces is added to an artifact that declares none"

  # A control file that ends with the paragraph terminator.  An added field
  # printed after it starts a second paragraph, and dpkg rejects the file for
  # a missing Package rather than reading the relationship.
  make_transition_fixture 'Conflicts: libglapi-amber
' "${transition_root}/terminated.deb"
  build_transition "${transition_root}/terminated.deb" \
    "${transition_root}/terminated-transition.deb" deepstream-9.1 \
    || fail "the installer could not add a field to a terminated paragraph"
  expect_equal "$(dpkg-deb -f "${transition_root}/terminated-transition.deb" Replaces)" \
    "deepstream-9.1" "an added field joins the paragraph instead of starting a second one"
  expect_equal "$(dpkg-deb -f "${transition_root}/terminated-transition.deb" Package)" \
    "deepstream-9.2" "the paragraph a trailing blank line ends is still the package's"

  # A relationship folded over continuation lines: the list belongs after the
  # last of them, not after the comma that ends the first.
  make_transition_fixture 'Conflicts: libglapi-amber,
 deepstream-binaries-x86' "${transition_root}/folded.deb"
  build_transition "${transition_root}/folded.deb" "${transition_root}/folded-transition.deb" \
    deepstream-9.1 || fail "the installer could not extend a folded relationship"
  expect_equal "$(dpkg-deb -f "${transition_root}/folded-transition.deb" Conflicts)" \
    "libglapi-amber, deepstream-binaries-x86, deepstream-9.1" \
    "a folded Conflicts is extended at its end"

  # dpkg reads a control file that starts with a blank line.  Treating that
  # line as the end of the stanza puts the added fields in one of their own,
  # with no Package in it.
  make_transition_fixture '' "${transition_root}/leading.deb"
  leading_root="${transition_root}/build/leading"
  printf '\n%s' "$(cat "${leading_root}/DEBIAN/control")" >"${leading_root}/DEBIAN/control.new"
  printf '\n' >>"${leading_root}/DEBIAN/control.new"
  mv "${leading_root}/DEBIAN/control.new" "${leading_root}/DEBIAN/control"
  dpkg-deb -Zzstd --build --root-owner-group "${leading_root}" \
    "${transition_root}/leading.deb" >/dev/null
  build_transition "${transition_root}/leading.deb" \
    "${transition_root}/leading-transition.deb" deepstream-9.1 \
    || fail "the installer could not rewrite a control file that starts blank"
  expect_equal "$(dpkg-deb -f "${transition_root}/leading-transition.deb" Package)" \
    "deepstream-9.2" "a blank first line does not end the stanza before it starts"
  expect_equal "$(dpkg-deb -f "${transition_root}/leading-transition.deb" Replaces)" \
    "deepstream-9.1" "the added field joins the package's own stanza"

  # dpkg tolerates a space before the colon.  Not recognizing the field adds a
  # second one, and a control file with two Conflicts does not parse.
  make_transition_fixture 'Conflicts : libglapi-amber' "${transition_root}/spaced.deb"
  build_transition "${transition_root}/spaced.deb" "${transition_root}/spaced-transition.deb" \
    deepstream-9.1 || fail "the installer could not extend a spaced field name"
  expect_equal "$(dpkg-deb -f "${transition_root}/spaced-transition.deb" Conflicts)" \
    "libglapi-amber, deepstream-9.1" "a space before the colon still names the field"

  # dpkg-deb -Znone writes control.tar and data.tar with no suffix after tar.
  make_transition_fixture 'Conflicts: libglapi-amber' "${transition_root}/plain.deb"
  dpkg-deb -Znone --build --root-owner-group "${transition_root}/build/plain" \
    "${transition_root}/plain.deb" >/dev/null
  build_transition "${transition_root}/plain.deb" "${transition_root}/plain-transition.deb" \
    deepstream-9.1 || fail "the installer rejected an uncompressed artifact"
  expect_equal "$(dpkg-deb -f "${transition_root}/plain-transition.deb" Replaces)" \
    "deepstream-9.1" "an uncompressed control member is rewritten like any other"

  # dpkg-deb --info and -f read the control member alone, so an artifact that
  # lost its payload would pass every field check the helper makes.
  cp "${transition_root}/nvidia.deb" "${transition_root}/payloadless.deb"
  ar d "${transition_root}/payloadless.deb" "$(ar t "${transition_root}/payloadless.deb" \
    | grep '^data[.]tar[.]')"
  if build_transition "${transition_root}/payloadless.deb" \
    "${transition_root}/payloadless-transition.deb" deepstream-9.1 >/dev/null 2>&1; then
    fail "the installer built a transition artifact out of a package with no payload"
  fi

  # With no release to name, the verification has nothing to look for and
  # would pass an artifact carrying two empty fields.  Assert the diagnostic:
  # without the guard this fixture fails later anyway, on the empty list it
  # writes into Conflicts, and the case would pass for the wrong reason.
  empty_output="$(build_transition "${transition_root}/nvidia.deb" \
    "${transition_root}/empty-transition.deb" 2>&1 1>/dev/null)" \
    && fail "the installer built a transition artifact against no release at all"
  expect_equal "${empty_output}" \
    "ERROR: no DeepStream release to declare the transition against." \
    "naming no release is refused before anything is built"
  [[ ! -e "${transition_root}/empty-transition.deb" ]] \
    || fail "the refused transition still left an artifact behind"

  # Which installed releases the copy has to name.  dpkg's current status is
  # what says a release still owns files: a held one and one an interrupted
  # apt already marked for removal both do.
  superseded_with() {
    HSTREAM_INSTALLER_SOURCE_ONLY=1 bash -c '
      # shellcheck disable=SC1090
      source "$1"
      superseded_deepstream_packages "$2"
    ' _ "${SCRIPT_DIR}/install_deb.sh" "$1"
  }
  # Checked assignments: an empty expectation would otherwise be satisfied by
  # the filter failing outright.
  expect_superseded() {
    local inventory="$1" expected="$2" label="$3" actual
    # %b: the fixtures carry dpkg's tab separator as an escape.
    actual="$(printf '%b' "${inventory}" | superseded_with deepstream-9.2)" \
      || fail "${label}: the filter failed"
    expect_equal "${actual}" "${expected}" "${label}"
  }
  expect_superseded 'deepstream-9.1:amd64\tii
deepstream-9.2:amd64\tii
' deepstream-9.1 "the release being installed is not superseded by itself"
  expect_superseded 'deepstream-9.1:amd64\thi
' deepstream-9.1 "a held release still owns its files"
  expect_superseded 'deepstream-9.1:amd64\tri
' deepstream-9.1 "a release marked for removal still owns its files until it is gone"
  expect_superseded 'deepstream-8.0:amd64\trc
deepstream-9.1:amd64\tpn
' "" "a release down to config files or gone entirely owns none"
  expect_superseded 'deepstream-sample-data\tii
deepstream-binaries-x86\tii
' "" "split packages are left to the artifact's own conflicts"

  rm -rf "${transition_root}"
  trap - EXIT
fi

for script in deploy.sh make_deb.sh make_deb_jetson.sh; do
  if output="$(JETSON_QT=invalid NODES=stubby bash "${SCRIPT_DIR}/${script}" 2>&1)"; then
    fail "${script} accepted an invalid JETSON_QT option"
  fi
  expect_equal "${output}" 'ERROR: JETSON_QT must be 0 or 1.' \
    "${script} rejects invalid Qt options before repository, SSH or package operations"
done

echo "deploy_test: PASS"
