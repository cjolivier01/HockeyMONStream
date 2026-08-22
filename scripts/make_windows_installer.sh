#!/bin/bash
# Build the small Windows/WSL bootstrapper on Linux with NSIS. The executable
# contains only provisioning scripts and the application icon; Ubuntu and
# HStream are downloaded and checksum-verified on the Windows host.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_TAG=""
OUTPUT_DIR="${TOPDIR}/dist/windows"
REPOSITORY="cjolivier01/hstream"
SIGNING_PKCS12="${WINDOWS_SIGNING_PKCS12:-}"
SIGNING_PASSWORD_FILE="${WINDOWS_SIGNING_PASSWORD_FILE:-}"
SIGNING_CA_FILE="${WINDOWS_SIGNING_CA_FILE:-}"
SIGNING_TIMESTAMP_URL="${WINDOWS_SIGNING_TIMESTAMP_URL:-http://timestamp.digicert.com}"

usage() {
  cat <<'USAGE'
Usage: scripts/make_windows_installer.sh [OPTIONS]

Options:
  --version vX.Y.Z     Release downloaded by the bootstrapper. Defaults to the
                       highest local semantic-version tag, or v0.1.0.
  --output-dir DIR     Output directory (default: dist/windows).
  --repository OWNER/REPO
                       GitHub release repository (default: cjolivier01/hstream).
  -h, --help           Show this help.

Ubuntu build dependencies:
  sudo apt-get install nsis librsvg2-bin icoutils

Release signing environment:
  WINDOWS_SIGNING_PKCS12          Authenticode code-signing PKCS#12 file.
  WINDOWS_SIGNING_PASSWORD_FILE   File containing its password (never placed
                                  on the command line).
  WINDOWS_SIGNING_CA_FILE         Optional PEM trust anchor used only to verify
                                  a deliberately private/self-signed signer.
  WINDOWS_SIGNING_TIMESTAMP_URL   RFC 3161 service (default: DigiCert).
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION_TAG="$2"; shift ;;
    --version=*) VERSION_TAG="${1#*=}" ;;
    --output-dir) OUTPUT_DIR="$2"; shift ;;
    --output-dir=*) OUTPUT_DIR="${1#*=}" ;;
    --repository) REPOSITORY="$2"; shift ;;
    --repository=*) REPOSITORY="${1#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

if [[ -z "${VERSION_TAG}" ]]; then
  VERSION_TAG="$(git -C "${TOPDIR}" tag --list 'v*' \
    | awk '/^v(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$/' \
    | sort -V | tail -n 1)"
  VERSION_TAG="${VERSION_TAG:-v0.1.0}"
fi
if [[ ! "${VERSION_TAG}" =~ ^v(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$ ]]; then
  echo "ERROR: --version must be a strict semantic version such as v0.1.0." >&2
  exit 2
fi
if [[ ! "${REPOSITORY}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
  echo "ERROR: --repository must be OWNER/REPO." >&2
  exit 2
fi
for command_name in makensis rsvg-convert icotool file sha256sum; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "ERROR: required Windows installer build command not found: ${command_name}" >&2
    echo "Install build dependencies with: sudo apt-get install nsis librsvg2-bin icoutils" >&2
    exit 1
  fi
done
if [[ -n "${SIGNING_PKCS12}" || -n "${SIGNING_PASSWORD_FILE}" ]]; then
  if [[ ! -f "${SIGNING_PKCS12}" || ! -f "${SIGNING_PASSWORD_FILE}" ]]; then
    echo "ERROR: WINDOWS_SIGNING_PKCS12 and WINDOWS_SIGNING_PASSWORD_FILE must both name readable files." >&2
    exit 1
  fi
  if ! command -v osslsigncode >/dev/null 2>&1; then
    echo "ERROR: Authenticode signing requires osslsigncode." >&2
    echo "Install it with: sudo apt-get install osslsigncode" >&2
    exit 1
  fi
fi
if [[ -n "${SIGNING_CA_FILE}" && ! -f "${SIGNING_CA_FILE}" ]]; then
  echo "ERROR: WINDOWS_SIGNING_CA_FILE must name a readable PEM certificate file." >&2
  exit 1
fi

OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"
build_dir="$(mktemp -d "${OUTPUT_DIR}/.hstream-windows-installer.XXXXXX")"
cleanup() {
  local status=$?
  set +e
  if [[ "${build_dir}" == "${OUTPUT_DIR}"/.hstream-windows-installer.* ]]; then
    rm -rf -- "${build_dir}"
  fi
  return "${status}"
}
trap cleanup EXIT

icon_pngs=()
for size in 16 24 32 48 64 128 256; do
  icon_png="${build_dir}/hstream-${size}.png"
  rsvg-convert --width "${size}" --height "${size}" \
    "${TOPDIR}/src/apps/hstream-ui/hstream-ui.svg" >"${icon_png}"
  icon_pngs+=("${icon_png}")
done
icon_file="${build_dir}/hstream.ico"
icotool --create --output="${icon_file}" "${icon_pngs[@]}"

package_version="${VERSION_TAG#v}"
output_file="${OUTPUT_DIR}/hstream_${VERSION_TAG}_windows-wsl-setup.exe"
temporary_output="${build_dir}/$(basename "${output_file}")"
powershell_sha256="$(sha256sum "${TOPDIR}/packaging/windows/hstream-wsl.ps1" | awk '{print toupper($1)}')"
makensis -V2 -NOCD \
  -DVERSION_TAG="${VERSION_TAG}" \
  -DPACKAGE_VERSION="${package_version}" \
  -DREPOSITORY="${REPOSITORY}" \
  -DOUTPUT_FILE="${temporary_output}" \
  -DICON_FILE="${icon_file}" \
  -DPOWERSHELL_SOURCE="${TOPDIR}/packaging/windows/hstream-wsl.ps1" \
  -DPOWERSHELL_SHA256="${powershell_sha256}" \
  -DLINUX_INSTALLER_SOURCE="${TOPDIR}/scripts/install_deb.sh" \
  "${TOPDIR}/packaging/windows/HStreamWslInstaller.nsi"

if [[ "$(file -Lb "${temporary_output}")" != *"PE32 executable"* ]]; then
  echo "ERROR: NSIS did not produce a Windows executable: ${temporary_output}" >&2
  exit 1
fi
installer_to_publish="${temporary_output}"
if [[ -n "${SIGNING_PKCS12}" ]]; then
  signed_output="${build_dir}/signed-$(basename "${output_file}")"
  osslsigncode sign \
    -pkcs12 "${SIGNING_PKCS12}" \
    -readpass "${SIGNING_PASSWORD_FILE}" \
    -h sha256 \
    -n "HStream Windows/WSL Bootstrapper" \
    -i "https://github.com/${REPOSITORY}" \
    -ts "${SIGNING_TIMESTAMP_URL}" \
    -in "${temporary_output}" \
    -out "${signed_output}"
  verify_args=(-in "${signed_output}")
  if [[ -n "${SIGNING_CA_FILE}" ]]; then
    verify_args=(-CAfile "${SIGNING_CA_FILE}" "${verify_args[@]}")
  fi
  osslsigncode verify "${verify_args[@]}"
  installer_to_publish="${signed_output}"
fi
install -m 0755 "${installer_to_publish}" "${output_file}"
echo "Windows WSL bootstrapper: ${output_file}"
du -h "${output_file}"
