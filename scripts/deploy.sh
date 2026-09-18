#!/usr/bin/env bash
# Deploy or remove HStream Debian packages on SSH nodes.
set -uo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSH_CONNECT_TIMEOUT="${HSTREAM_DEPLOY_SSH_CONNECT_TIMEOUT:-10}"
DEPLOY_OUTPUT_DIR="${DEPLOY_OUTPUT_DIR:-${TOPDIR}/dist}"
NODES_CSV="${NODES:-}"
REQUESTED_PACKAGE_VERSION="${PACKAGE_VERSION:-}"
REQUESTED_DEEPSTREAM_DEB="${DEEPSTREAM_DEB:-}"
OPERATION="deploy"

declare -a NODES_LIST=()
declare -a TARGET_KEYS=()
declare -A SEEN_NODES=()
declare -A SEEN_TARGETS=()
declare -A NODE_OS=()
declare -A NODE_PLATFORM=()
declare -A NODE_TARGET=()
declare -A NODE_PREVIOUS=()
declare -A NODE_PREVIOUS_PACKAGE=()
declare -A NODE_INSTALLED=()
declare -A NODE_RESULT=()
declare -A NODE_DETAIL=()
declare -A TARGET_BUILD_HOST=()
declare -A TARGET_DEB=()
declare -A TARGET_INSTALLER=()
declare -A TARGET_BUILD_ERROR=()

PACKAGE_VERSION_NORMALIZED=""
DESKTOP_DEEPSTREAM_DEB=""

usage() {
  cat <<'USAGE'
Usage:
  make deploy NODES=node1,node2[,node3]
  make undeploy NODES=node1,node2[,node3]

The deployment runner detects each node over SSH, builds one package for each
distinct supported target, and force-installs the matching package. Supported
targets are Ubuntu 24.04/26.04 amd64 desktops and Ubuntu 22.04 arm64 Jetsons.
Nodes must allow non-interactive SSH access and passwordless sudo.
The invoking HStream repository must have no tracked or source-file changes.

Undeploy removes and purges the hstream package and its legacy hmstream name.
It leaves DeepStream and shared dependency packages installed.

Optional make variables:
  PACKAGE_VERSION=VERSION   Override the source-derived package version.
  DEEPSTREAM_DEB=FILE      Use this DeepStream 9.1 amd64 artifact for desktops.
  DEPLOY_OUTPUT_DIR=DIR    Override the package output root (default: dist).
USAGE
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "${value}"
}

parse_nodes() {
  local nodes_csv="$1"
  local -a raw_nodes=()
  local raw_node node

  NODES_LIST=()
  SEEN_NODES=()
  if [[ -z "$(trim "${nodes_csv}")" ]]; then
    printf 'ERROR: NODES is required. Usage: make %s NODES=monster,stubby,mini\n' "${OPERATION}" >&2
    return 2
  fi
  if [[ "${nodes_csv}" == ,* || "${nodes_csv}" == *, || "${nodes_csv}" == *,,* ]]; then
    printf 'ERROR: NODES contains an empty node name: %s\n' "${nodes_csv}" >&2
    return 2
  fi

  IFS=',' read -r -a raw_nodes <<< "${nodes_csv}"
  for raw_node in "${raw_nodes[@]}"; do
    node="$(trim "${raw_node}")"
    if [[ -z "${node}" ]]; then
      printf 'ERROR: NODES contains an empty node name: %s\n' "${nodes_csv}" >&2
      return 2
    fi
    if [[ ! "${node}" =~ ^[A-Za-z0-9_.-]+(@[A-Za-z0-9_.-]+)?$ || "${node}" == -* ]]; then
      printf 'ERROR: invalid SSH node in NODES: %s\n' "${node}" >&2
      return 2
    fi
    if [[ -n "${SEEN_NODES[${node}]+set}" ]]; then
      printf 'ERROR: duplicate node in NODES: %s\n' "${node}" >&2
      return 2
    fi
    SEEN_NODES["${node}"]=1
    NODES_LIST+=("${node}")
  done
}

classify_target() {
  local os_id="$1"
  local os_version="$2"
  local architecture="$3"
  local platform="$4"

  case "${platform}:${os_id}:${os_version}:${architecture}" in
    desktop:ubuntu:24.04:x86_64) printf 'desktop-ubuntu24.04-amd64' ;;
    desktop:ubuntu:26.04:x86_64) printf 'desktop-ubuntu26.04-amd64' ;;
    jetson:ubuntu:22.04:aarch64) printf 'jetson-ubuntu22.04-arm64' ;;
    *) return 1 ;;
  esac
}

normalize_package_version() {
  local version="$1"
  version="${version#v}"
  version="$(printf '%s' "${version}" | sed -E 's/[^A-Za-z0-9.+:~-]+/./g; s/[.]+/./g; s/^[.]+//; s/[.]+$//')"
  if [[ ! "${version}" =~ ^[0-9] ]]; then
    version="0.0+git.${version}"
  fi
  printf '%s' "${version}"
}

target_description() {
  case "$1" in
    desktop-ubuntu24.04-amd64) printf 'desktop/amd64' ;;
    desktop-ubuntu26.04-amd64) printf 'desktop/amd64' ;;
    jetson-ubuntu22.04-arm64) printf 'jetson/arm64' ;;
    *) printf 'unsupported' ;;
  esac
}

deployment_action() {
  local previous="$1"
  local installed="$2"
  local previous_package="${3:-hstream}"
  if [[ -z "${previous}" ]]; then
    printf 'installed'
  elif [[ "${previous_package}" != hstream ]]; then
    printf 'replaced legacy %s' "${previous_package}"
  elif [[ "${previous}" == "${installed}" ]]; then
    printf 'reinstalled'
  elif dpkg --compare-versions "${previous}" lt "${installed}"; then
    printf 'updated'
  elif dpkg --compare-versions "${previous}" gt "${installed}"; then
    printf 'downgraded'
  else
    printf 'replaced'
  fi
}

require_local_commands() {
  local command_name
  local -a required_commands=(git ssh sed)
  if [[ "${OPERATION}" == deploy ]]; then
    required_commands+=(scp dpkg dpkg-deb)
  fi
  for command_name in "${required_commands[@]}"; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
      printf 'ERROR: required command not found: %s\n' "${command_name}" >&2
      return 1
    fi
  done
  if [[ ! "${SSH_CONNECT_TIMEOUT}" =~ ^[1-9][0-9]*$ ]]; then
    printf 'ERROR: HSTREAM_DEPLOY_SSH_CONNECT_TIMEOUT must be a positive integer.\n' >&2
    return 1
  fi
}

require_clean_repository() {
  local unexpected_untracked
  if ! git -C "${TOPDIR}" diff --quiet HEAD -- || ! git -C "${TOPDIR}" diff --cached --quiet; then
    printf 'ERROR: refusing to %s with tracked or staged changes.\n' "${OPERATION}" >&2
    return 1
  fi
  unexpected_untracked="$({
    git -C "${TOPDIR}" ls-files --others --exclude-standard \
      | grep -Ev '^(bazelisk|run|stitching-calibration-note[.]txt|dist/|dist-staging/|output_workdirs/|bazel-[^/]+(/|$))'
  } || true)"
  if [[ -n "${unexpected_untracked}" ]]; then
    printf 'ERROR: refusing to %s with untracked source files:\n' "${OPERATION}" >&2
    while IFS= read -r source_path; do
      printf '  %s\n' "${source_path}" >&2
    done <<< "${unexpected_untracked}"
    return 1
  fi
}

detect_node() {
  local node="$1"
  local output identity marker os_id os_version architecture platform previous_package previous extra target_key

  printf '\n[%s] Detecting %s...\n' "${OPERATION}" "${node}"
  if ! output="$(ssh -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${node}" bash -s <<'REMOTE_DETECT'
set -eu
if [ ! -r /etc/os-release ]; then
  echo "ERROR: /etc/os-release is unavailable" >&2
  exit 1
fi
. /etc/os-release
platform=desktop
if [ -f /etc/nv_tegra_release ]; then
  platform=jetson
elif [ -r /proc/device-tree/compatible ] && tr '\000' '\n' < /proc/device-tree/compatible | grep -qi tegra; then
  platform=jetson
fi
installed_package=""
installed_version=""
for candidate in hstream hmstream; do
  candidate_status="$(dpkg-query -W -f='${db:Status-Status}' "${candidate}" 2>/dev/null || true)"
  if [ "${candidate_status}" = installed ]; then
    installed_package="${candidate}"
    installed_version="$(dpkg-query -W -f='${Version}' "${candidate}")"
    break
  fi
done
printf '__HSTREAM_DEPLOY__|%s|%s|%s|%s|%s|%s\n' \
  "${ID:-}" "${VERSION_ID:-}" "$(uname -m)" "${platform}" "${installed_package}" "${installed_version}"
REMOTE_DETECT
  )"; then
    NODE_OS["${node}"]="unknown"
    NODE_PLATFORM["${node}"]="unknown"
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="SSH or OS detection failed"
    return 1
  fi

  identity="$(printf '%s\n' "${output}" | sed -n '/^__HSTREAM_DEPLOY__|/p' | tail -n 1)"
  if [[ -z "${identity}" ]]; then
    NODE_OS["${node}"]="unknown"
    NODE_PLATFORM["${node}"]="unknown"
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="invalid OS detection response"
    return 1
  fi
  IFS='|' read -r marker os_id os_version architecture platform previous_package previous extra <<< "${identity}"
  if [[ "${marker}" != "__HSTREAM_DEPLOY__" || -n "${extra:-}" ]]; then
    NODE_OS["${node}"]="unknown"
    NODE_PLATFORM["${node}"]="unknown"
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="invalid OS detection fields"
    return 1
  fi

  NODE_OS["${node}"]="${os_id:-unknown} ${os_version:-unknown}"
  NODE_PLATFORM["${node}"]="${platform:-unknown}"
  NODE_PREVIOUS_PACKAGE["${node}"]="${previous_package:-}"
  NODE_PREVIOUS["${node}"]="${previous:-}"
  NODE_INSTALLED["${node}"]="${previous:-}"

  if ! target_key="$(classify_target "${os_id}" "${os_version}" "${architecture}" "${platform}")"; then
    NODE_TARGET["${node}"]="unsupported"
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="unsupported ${os_id:-OS} ${os_version:-version} ${platform:-platform}/${architecture:-architecture}"
    printf '[%s] ERROR: %s: %s\n' "${OPERATION}" "${node}" "${NODE_DETAIL[${node}]}" >&2
    return 1
  fi

  NODE_TARGET["${node}"]="${target_key}"
  NODE_PLATFORM["${node}"]="$(target_description "${target_key}")"
  if [[ -z "${SEEN_TARGETS[${target_key}]+set}" ]]; then
    SEEN_TARGETS["${target_key}"]=1
    TARGET_KEYS+=("${target_key}")
    TARGET_BUILD_HOST["${target_key}"]="${node}"
  fi
  printf '[%s] %s: %s (%s), installed HStream: %s\n' \
    "${OPERATION}" "${node}" "${NODE_OS[${node}]}" "${NODE_PLATFORM[${node}]}" \
    "${previous_package:+${previous_package} }${previous:-not installed}"
}

resolve_desktop_deepstream_deb() {
  local candidate version selected_candidate="" selected_version=""
  local -a candidates=()

  if [[ -n "${REQUESTED_DEEPSTREAM_DEB}" ]]; then
    candidates+=("${REQUESTED_DEEPSTREAM_DEB}")
  else
    shopt -s nullglob
    candidates=("${TOPDIR}/../DeepStream/artifacts/"deepstream-9.1_*_amd64.deb)
    shopt -u nullglob
  fi

  for candidate in "${candidates[@]}"; do
    [[ -f "${candidate}" ]] || continue
    if [[ "$(dpkg-deb -f "${candidate}" Package 2>/dev/null || true)" != "deepstream-9.1" ||
          "$(dpkg-deb -f "${candidate}" Architecture 2>/dev/null || true)" != "amd64" ]]; then
      continue
    fi
    version="$(dpkg-deb -f "${candidate}" Version 2>/dev/null || true)"
    if dpkg --compare-versions "${version}" ge 9.1.0-1 && dpkg --compare-versions "${version}" lt '9.2~'; then
      if [[ -z "${selected_version}" ]] || dpkg --compare-versions "${version}" gt "${selected_version}"; then
        selected_candidate="${candidate}"
        selected_version="${version}"
      fi
    fi
  done
  if [[ -n "${selected_candidate}" ]]; then
    DESKTOP_DEEPSTREAM_DEB="$(readlink -f "${selected_candidate}")"
    return 0
  fi

  if [[ -n "${REQUESTED_DEEPSTREAM_DEB}" ]]; then
    printf 'ERROR: DEEPSTREAM_DEB is not a supported DeepStream 9.1 amd64 package: %s\n' \
      "${REQUESTED_DEEPSTREAM_DEB}" >&2
  else
    printf 'ERROR: no DeepStream 9.1 amd64 artifact found under %s.\n' \
      "${TOPDIR}/../DeepStream/artifacts" >&2
    printf 'Pass DEEPSTREAM_DEB=/path/to/deepstream-9.1_*_amd64.deb.\n' >&2
  fi
  return 1
}

verify_built_package() {
  local deb="$1"
  local expected_os="$2"
  local expected_platform="$3"
  local expected_arch="$4"
  local actual_package actual_version actual_os actual_platform actual_arch

  if [[ ! -f "${deb}" ]]; then
    printf 'ERROR: expected package was not produced: %s\n' "${deb}" >&2
    return 1
  fi
  actual_package="$(dpkg-deb -f "${deb}" Package 2>/dev/null || true)"
  actual_version="$(dpkg-deb -f "${deb}" Version 2>/dev/null || true)"
  actual_os="$(dpkg-deb -f "${deb}" X-HStream-Target-Ubuntu 2>/dev/null || true)"
  actual_platform="$(dpkg-deb -f "${deb}" X-HStream-Target-Platform 2>/dev/null || true)"
  actual_arch="$(dpkg-deb -f "${deb}" Architecture 2>/dev/null || true)"
  if [[ "${actual_package}" != hstream || "${actual_version}" != "${PACKAGE_VERSION_NORMALIZED}" ||
        "${actual_os}" != "${expected_os}" || "${actual_platform}" != "${expected_platform}" ||
        "${actual_arch}" != "${expected_arch}" ]]; then
    printf 'ERROR: package metadata does not match deployment target: %s\n' "${deb}" >&2
    printf '  got package=%s version=%s ubuntu=%s platform=%s arch=%s\n' \
      "${actual_package:-unknown}" "${actual_version:-unknown}" "${actual_os:-unknown}" \
      "${actual_platform:-unknown}" "${actual_arch:-unknown}" >&2
    return 1
  fi
}

build_target() {
  local target_key="$1"
  local output_dir deb installer build_host

  installer="${TOPDIR}/scripts/install_deb.sh"

  case "${target_key}" in
    desktop-ubuntu24.04-amd64)
      output_dir="${DEPLOY_OUTPUT_DIR}/ubuntu24.04"
      deb="${output_dir}/hstream_${PACKAGE_VERSION_NORMALIZED}_amd64.deb"
      printf '\n[deploy] Building HStream %s for Ubuntu 24.04 amd64...\n' "${PACKAGE_VERSION_NORMALIZED}"
      if ! "${TOPDIR}/scripts/make_deb_docker.sh" \
        --target-ubuntu=24.04 \
        --output-dir="${output_dir}" \
        --version="${PACKAGE_VERSION_NORMALIZED}" \
        --deepstream-deb="${DESKTOP_DEEPSTREAM_DEB}"; then
        TARGET_BUILD_ERROR["${target_key}"]="package build failed"
        return 1
      fi
      if ! verify_built_package "${deb}" 24.04 desktop amd64 || [[ ! -x "${installer}" ]]; then
        TARGET_BUILD_ERROR["${target_key}"]="package artifact validation failed"
        return 1
      fi
      TARGET_DEB["${target_key}"]="${deb}"
      TARGET_INSTALLER["${target_key}"]="${installer}"
      ;;
    desktop-ubuntu26.04-amd64)
      output_dir="${DEPLOY_OUTPUT_DIR}/ubuntu26.04"
      deb="${output_dir}/hstream_${PACKAGE_VERSION_NORMALIZED}_amd64.deb"
      printf '\n[deploy] Building HStream %s for Ubuntu 26.04 amd64...\n' "${PACKAGE_VERSION_NORMALIZED}"
      if ! "${TOPDIR}/scripts/make_deb_docker.sh" \
        --target-ubuntu=26.04 \
        --output-dir="${output_dir}" \
        --version="${PACKAGE_VERSION_NORMALIZED}" \
        --deepstream-deb="${DESKTOP_DEEPSTREAM_DEB}"; then
        TARGET_BUILD_ERROR["${target_key}"]="package build failed"
        return 1
      fi
      if ! verify_built_package "${deb}" 26.04 desktop amd64 || [[ ! -x "${installer}" ]]; then
        TARGET_BUILD_ERROR["${target_key}"]="package artifact validation failed"
        return 1
      fi
      TARGET_DEB["${target_key}"]="${deb}"
      TARGET_INSTALLER["${target_key}"]="${installer}"
      ;;
    jetson-ubuntu22.04-arm64)
      output_dir="${DEPLOY_OUTPUT_DIR}/jetson"
      deb="${output_dir}/hstream_${PACKAGE_VERSION_NORMALIZED}_arm64.deb"
      build_host="${TARGET_BUILD_HOST[${target_key}]}"
      printf '\n[deploy] Building HStream %s for Jetson on %s...\n' \
        "${PACKAGE_VERSION_NORMALIZED}" "${build_host}"
      if ! "${TOPDIR}/scripts/make_deb_jetson.sh" \
        --host="${build_host}" \
        --output-dir="${output_dir}" \
        --version="${PACKAGE_VERSION_NORMALIZED}"; then
        TARGET_BUILD_ERROR["${target_key}"]="package build failed on ${build_host}"
        return 1
      fi
      if ! verify_built_package "${deb}" 22.04 jetson arm64; then
        TARGET_BUILD_ERROR["${target_key}"]="package artifact validation failed"
        return 1
      fi
      TARGET_DEB["${target_key}"]="${deb}"
      ;;
    *)
      TARGET_BUILD_ERROR["${target_key}"]="unsupported build target"
      return 1
      ;;
  esac
}

query_installed_version() {
  local node="$1"
  local output version
  if ! output="$(ssh -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${node}" \
    "status=\$(dpkg-query -W -f='\${db:Status-Status}' hstream 2>/dev/null || true); version=; if [ \"\${status}\" = installed ]; then version=\$(dpkg-query -W -f='\${Version}' hstream); fi; printf '__HSTREAM_VERSION__|%s\\n' \"\${version}\"")"; then
    return 1
  fi
  version="$(printf '%s\n' "${output}" | sed -n 's/^__HSTREAM_VERSION__|//p' | tail -n 1)"
  printf '%s' "${version}"
}

create_remote_directory() {
  local node="$1"
  local output remote_dir
  if ! output="$(ssh -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${node}" \
    'mktemp -d /tmp/hstream-deploy.XXXXXX')"; then
    return 1
  fi
  remote_dir="$(printf '%s\n' "${output}" | tail -n 1)"
  if [[ ! "${remote_dir}" =~ ^/tmp/hstream-deploy\.[A-Za-z0-9]+$ ]]; then
    printf 'ERROR: %s returned an unsafe deployment directory: %s\n' "${node}" "${remote_dir}" >&2
    return 1
  fi
  printf '%s' "${remote_dir}"
}

cleanup_remote_directory() {
  local node="$1"
  local remote_dir="$2"
  [[ "${remote_dir}" =~ ^/tmp/hstream-deploy\.[A-Za-z0-9]+$ ]] || return 0
  ssh -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${node}" \
    "rm -rf -- '${remote_dir}'" >/dev/null 2>&1 || true
}

deploy_desktop() {
  local node="$1"
  local deb="$2"
  local installer="$3"
  local remote_dir deb_name installer_name deepstream_name status=0

  if ! remote_dir="$(create_remote_directory "${node}")"; then
    return 1
  fi
  deb_name="$(basename "${deb}")"
  installer_name="$(basename "${installer}")"
  deepstream_name="$(basename "${DESKTOP_DEEPSTREAM_DEB}")"

  if ! scp -q -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" \
    "${deb}" "${installer}" "${DESKTOP_DEEPSTREAM_DEB}" "${node}:${remote_dir}/"; then
    status=1
  elif ! ssh -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${node}" bash -s -- \
    "${remote_dir}" "${installer_name}" "${deepstream_name}" "${deb_name}" <<'REMOTE_INSTALL_DESKTOP'
set -euo pipefail
remote_dir="$1"
installer_name="$2"
deepstream_name="$3"
deb_name="$4"
chmod 0755 "${remote_dir}/${installer_name}"
sudo -n "${remote_dir}/${installer_name}" \
  --force-hstream \
  --deepstream-deb="${remote_dir}/${deepstream_name}" \
  --hstream-deb="${remote_dir}/${deb_name}"
REMOTE_INSTALL_DESKTOP
  then
    status=1
  fi
  cleanup_remote_directory "${node}" "${remote_dir}"
  return "${status}"
}

deploy_jetson() {
  local node="$1"
  local deb="$2"
  local remote_dir deb_name status=0

  if ! remote_dir="$(create_remote_directory "${node}")"; then
    return 1
  fi
  deb_name="$(basename "${deb}")"

  if ! scp -q -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" \
    "${deb}" "${node}:${remote_dir}/"; then
    status=1
  elif ! ssh -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${node}" bash -s -- \
    "${remote_dir}" "${deb_name}" <<'REMOTE_INSTALL_JETSON'
set -euo pipefail
remote_dir="$1"
deb_name="$2"
export DEBIAN_FRONTEND=noninteractive
sudo -n apt-get install -y --no-install-recommends --reinstall --allow-downgrades \
  "${remote_dir}/${deb_name}"
sudo -n apt-get check
REMOTE_INSTALL_JETSON
  then
    status=1
  fi
  cleanup_remote_directory "${node}" "${remote_dir}"
  return "${status}"
}

deploy_node() {
  local node="$1"
  local target_key="${NODE_TARGET[${node}]}"
  local deb="${TARGET_DEB[${target_key}]:-}"
  local installed action

  if [[ -z "${deb}" ]]; then
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="${TARGET_BUILD_ERROR[${target_key}]:-package unavailable}"
    return 1
  fi

  printf '\n[deploy] Installing %s on %s...\n' "$(basename "${deb}")" "${node}"
  case "${target_key}" in
    desktop-*)
      if ! deploy_desktop "${node}" "${deb}" "${TARGET_INSTALLER[${target_key}]}"; then
        NODE_RESULT["${node}"]="FAILED"
        NODE_DETAIL["${node}"]="upload or installation failed"
        return 1
      fi
      ;;
    jetson-*)
      if ! deploy_jetson "${node}" "${deb}"; then
        NODE_RESULT["${node}"]="FAILED"
        NODE_DETAIL["${node}"]="upload or installation failed"
        return 1
      fi
      ;;
  esac

  if ! installed="$(query_installed_version "${node}")" || [[ "${installed}" != "${PACKAGE_VERSION_NORMALIZED}" ]]; then
    NODE_INSTALLED["${node}"]="${installed:-unknown}"
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="post-install version verification failed"
    return 1
  fi

  NODE_INSTALLED["${node}"]="${installed}"
  action="$(deployment_action \
    "${NODE_PREVIOUS[${node}]:-}" "${installed}" "${NODE_PREVIOUS_PACKAGE[${node}]:-hstream}")"
  NODE_RESULT["${node}"]="OK"
  NODE_DETAIL["${node}"]="${action}"
  printf '[deploy] %s: HStream %s (%s).\n' "${node}" "${installed}" "${action}"
}

undeploy_node() {
  local node="$1"
  local installed

  if [[ -z "${NODE_PREVIOUS[${node}]:-}" ]]; then
    NODE_INSTALLED["${node}"]=""
    NODE_RESULT["${node}"]="OK"
    NODE_DETAIL["${node}"]="already absent"
    printf '[undeploy] %s: HStream is already absent.\n' "${node}"
    return 0
  fi

  printf '\n[undeploy] Removing %s %s from %s...\n' \
    "${NODE_PREVIOUS_PACKAGE[${node}]:-hstream}" "${NODE_PREVIOUS[${node}]}" "${node}"
  if ! ssh -o BatchMode=yes -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${node}" bash -s <<'REMOTE_UNDEPLOY'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
packages=()
for package in hstream hmstream; do
  status="$(dpkg-query -W -f='${db:Status-Status}' "${package}" 2>/dev/null || true)"
  if [ "${status}" = installed ]; then packages+=("${package}"); fi
done
if [ "${#packages[@]}" -gt 0 ]; then
  sudo -n apt-get remove --purge -y "${packages[@]}"
fi
for package in hstream hmstream; do
  status="$(dpkg-query -W -f='${db:Status-Status}' "${package}" 2>/dev/null || true)"
  if [ "${status}" = installed ]; then
    echo "ERROR: ${package} remains installed after package removal." >&2
    exit 1
  fi
done
REMOTE_UNDEPLOY
  then
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="package removal failed"
    return 1
  fi

  if ! installed="$(query_installed_version "${node}")"; then
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="post-removal verification failed"
    return 1
  fi
  if [[ -n "${installed}" ]]; then
    NODE_INSTALLED["${node}"]="${installed}"
    NODE_RESULT["${node}"]="FAILED"
    NODE_DETAIL["${node}"]="HStream ${installed} remains installed"
    return 1
  fi

  NODE_INSTALLED["${node}"]=""
  NODE_RESULT["${node}"]="OK"
  NODE_DETAIL["${node}"]="removed"
  printf '[undeploy] %s: HStream removed.\n' "${node}"
}

print_summary() {
  local node os platform previous previous_package installed result
  local node_width=4 os_width=2 platform_width=8 previous_width=8 installed_width=9 result_width=6

  for node in "${NODES_LIST[@]}"; do
    previous="${NODE_PREVIOUS[${node}]:---}"
    previous_package="${NODE_PREVIOUS_PACKAGE[${node}]:-}"
    if [[ -n "${previous_package}" && "${previous_package}" != hstream && "${previous}" != -- ]]; then
      previous="${previous_package}@${previous}"
    fi
    installed="${NODE_INSTALLED[${node}]:---}"
    os="${NODE_OS[${node}]:-unknown}"
    platform="${NODE_PLATFORM[${node}]:-unknown}"
    result="${NODE_RESULT[${node}]:-FAILED}: ${NODE_DETAIL[${node}]:-not attempted}"
    (( ${#node} > node_width )) && node_width=${#node}
    (( ${#os} > os_width )) && os_width=${#os}
    (( ${#platform} > platform_width )) && platform_width=${#platform}
    (( ${#previous} > previous_width )) && previous_width=${#previous}
    (( ${#installed} > installed_width )) && installed_width=${#installed}
    (( ${#result} > result_width )) && result_width=${#result}
  done

  if [[ "${OPERATION}" == undeploy ]]; then
    printf '\nUndeployment summary\n'
  else
    printf '\nDeployment summary\n'
  fi
  printf '%-*s  %-*s  %-*s  %-*s  %-*s  %-*s\n' \
    "${node_width}" NODE "${os_width}" OS "${platform_width}" TARGET \
    "${previous_width}" PREVIOUS "${installed_width}" INSTALLED "${result_width}" RESULT
  printf '%*s  %*s  %*s  %*s  %*s  %*s\n' \
    "${node_width}" '' "${os_width}" '' "${platform_width}" '' \
    "${previous_width}" '' "${installed_width}" '' "${result_width}" '' | tr ' ' '-'
  for node in "${NODES_LIST[@]}"; do
    previous="${NODE_PREVIOUS[${node}]:---}"
    previous_package="${NODE_PREVIOUS_PACKAGE[${node}]:-}"
    if [[ -n "${previous_package}" && "${previous_package}" != hstream && "${previous}" != -- ]]; then
      previous="${previous_package}@${previous}"
    fi
    installed="${NODE_INSTALLED[${node}]:---}"
    result="${NODE_RESULT[${node}]:-FAILED}: ${NODE_DETAIL[${node}]:-not attempted}"
    printf '%-*s  %-*s  %-*s  %-*s  %-*s  %-*s\n' \
      "${node_width}" "${node}" \
      "${os_width}" "${NODE_OS[${node}]:-unknown}" \
      "${platform_width}" "${NODE_PLATFORM[${node}]:-unknown}" \
      "${previous_width}" "${previous}" \
      "${installed_width}" "${installed}" \
      "${result_width}" "${result}"
  done
}

main() {
  local target_key node source_epoch source_hash overall_status=0 needs_desktop=0

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --nodes)
        if [[ $# -lt 2 ]]; then
          printf 'ERROR: --nodes requires a comma-separated value.\n' >&2
          return 2
        fi
        NODES_CSV="$2"
        shift
        ;;
      --nodes=*) NODES_CSV="${1#*=}" ;;
      --undeploy) OPERATION="undeploy" ;;
      -h|--help) usage; return 0 ;;
      *) printf 'ERROR: unknown option: %s\n' "$1" >&2; usage >&2; return 2 ;;
    esac
    shift
  done

  parse_nodes "${NODES_CSV}" || return $?
  require_local_commands || return 1
  require_clean_repository || return 1

  for node in "${NODES_LIST[@]}"; do
    if ! detect_node "${node}"; then
      overall_status=1
    fi
  done

  if [[ "${OPERATION}" == undeploy ]]; then
    for node in "${NODES_LIST[@]}"; do
      if [[ -n "${NODE_RESULT[${node}]:-}" ]]; then continue; fi
      if ! undeploy_node "${node}"; then overall_status=1; fi
    done
    print_summary
    return "${overall_status}"
  fi

  if [[ -z "${REQUESTED_PACKAGE_VERSION}" ]]; then
    if ! source_epoch="$(git -C "${TOPDIR}" show -s --format=%ct HEAD)" ||
       ! source_hash="$(git -C "${TOPDIR}" rev-parse --short=7 HEAD)"; then
      printf 'ERROR: could not derive a package version from Git HEAD.\n' >&2
      return 1
    fi
    REQUESTED_PACKAGE_VERSION="0.0.${source_epoch}+git.${source_hash}"
  fi
  PACKAGE_VERSION_NORMALIZED="$(normalize_package_version "${REQUESTED_PACKAGE_VERSION}")"
  if [[ -z "${PACKAGE_VERSION_NORMALIZED}" ]] || ! dpkg --validate-version "${PACKAGE_VERSION_NORMALIZED}" >/dev/null 2>&1; then
    printf 'ERROR: invalid package version: %s\n' "${REQUESTED_PACKAGE_VERSION}" >&2
    return 2
  fi

  for target_key in "${TARGET_KEYS[@]}"; do
    if [[ "${target_key}" == desktop-* ]]; then needs_desktop=1; fi
    if [[ "${target_key}" == jetson-* &&
          ! "${PACKAGE_VERSION_NORMALIZED}" =~ ^[0-9]+([.][0-9]+){2}([+~.-][A-Za-z0-9.+:~-]+)?$ ]]; then
      TARGET_BUILD_ERROR["${target_key}"]="Jetson package version must have MAJOR.MINOR.PATCH form"
      overall_status=1
    fi
  done

  if [[ "${needs_desktop}" -eq 1 ]] && ! resolve_desktop_deepstream_deb; then
    for target_key in "${TARGET_KEYS[@]}"; do
      if [[ "${target_key}" == desktop-* ]]; then
        TARGET_BUILD_ERROR["${target_key}"]="DeepStream build artifact unavailable"
      fi
    done
    overall_status=1
  fi

  for target_key in "${TARGET_KEYS[@]}"; do
    if [[ -n "${TARGET_BUILD_ERROR[${target_key}]:-}" ]]; then continue; fi
    if ! build_target "${target_key}"; then overall_status=1; fi
  done

  for node in "${NODES_LIST[@]}"; do
    if [[ -n "${NODE_RESULT[${node}]:-}" ]]; then continue; fi
    if ! deploy_node "${node}"; then overall_status=1; fi
  done

  print_summary
  return "${overall_status}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  trap 'exit 130' INT
  trap 'exit 143' TERM
  main "$@"
fi
