#!/bin/bash
# Build the Hugin command-line tools required by native stitching on
# JetPack 6. Ubuntu 22.04 does not publish hugin-tools for arm64, so release
# packages bundle these pinned, source-built executables and their private
# libraries instead of declaring an unsatisfiable Debian dependency.
set -euo pipefail

HUGIN_VERSION=2022.0.0
HUGIN_SHA256=5613b86e063d9e236100ae0f717aaa1cb5a183830ff8b985cd4b92e7aa607f8b
HUGIN_URL="https://deb.debian.org/debian/pool/main/h/hugin/hugin_${HUGIN_VERSION}+dfsg.orig.tar.xz"
VIGRA_VERSION=1.11.1
VIGRA_SHA256=5ddbfb435da7bd12536c7181ce3c7825ab4bea91d0c1518a952cebba445da6c0
VIGRA_URL="https://deb.debian.org/debian/pool/main/libv/libvigraimpex/libvigraimpex_${VIGRA_VERSION}+dfsg.orig.tar.xz"
CACHE_FORMAT=hugin-2022-vigra-1.11.1-r2

OUTPUT_DIR=""
EXPORT_SOURCES_DIR=""
CACHE_DIR="${HOME}/.cache/hstream/jetson-hugin-tools"

usage() {
  cat <<'USAGE'
Usage: scripts/build_hugin_tools_jetson.sh [--output-dir=DIR] [--export-sources=DIR] [--cache-dir=DIR]

Builds pinned native arm64 copies of autooptimiser, pano_modify, and nona plus
their Hugin and VIGRA runtime libraries. --export-sources copies the exact
verified source archives used by the build and works on any architecture for
release staging.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir) OUTPUT_DIR="$2"; shift ;;
    --output-dir=*) OUTPUT_DIR="${1#*=}" ;;
    --export-sources) EXPORT_SOURCES_DIR="$2"; shift ;;
    --export-sources=*) EXPORT_SOURCES_DIR="${1#*=}" ;;
    --cache-dir) CACHE_DIR="$2"; shift ;;
    --cache-dir=*) CACHE_DIR="${1#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

if [[ -z "${OUTPUT_DIR}" && -z "${EXPORT_SOURCES_DIR}" ]]; then
  echo "ERROR: --output-dir and/or --export-sources is required." >&2
  exit 2
fi
for command_name in curl sha256sum; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "ERROR: required Hugin source command not found: ${command_name}" >&2
    exit 1
  fi
done

CACHE_DIR="$(mkdir -p "${CACHE_DIR}" && cd "${CACHE_DIR}" && pwd)"
downloads="${CACHE_DIR}/downloads"
cached_artifacts="${CACHE_DIR}/${CACHE_FORMAT}"
mkdir -p "${downloads}"

cache_is_valid() {
  [[ -f "${cached_artifacts}/SHA256SUMS" ]] || return 1
  (
    cd "${cached_artifacts}"
    sha256sum --check --status SHA256SUMS
  ) || return 1
  [[ -x "${cached_artifacts}/bin/autooptimiser" &&
     -x "${cached_artifacts}/bin/pano_modify" &&
     -x "${cached_artifacts}/bin/nona" &&
     -f "${cached_artifacts}/lib/libhuginbase.so.0.0" &&
     -f "${cached_artifacts}/lib/libvigraimpex.so.11.1.11.1" &&
     -f "${cached_artifacts}/licenses/hugin/COPYING.txt" &&
     -f "${cached_artifacts}/licenses/vigra/LICENSE.txt" ]]
}

copy_cached_artifacts() {
  cp -a "${cached_artifacts}/." "${OUTPUT_DIR}/"
  LD_LIBRARY_PATH="${OUTPUT_DIR}/lib" "${OUTPUT_DIR}/bin/autooptimiser" --help >/dev/null
  LD_LIBRARY_PATH="${OUTPUT_DIR}/lib" "${OUTPUT_DIR}/bin/pano_modify" --help >/dev/null
  LD_LIBRARY_PATH="${OUTPUT_DIR}/lib" "${OUTPUT_DIR}/bin/nona" --help >/dev/null
}

fetch_verified() {
  local url="$1"
  local expected_hash="$2"
  local destination="$3"
  local current_hash=""
  if [[ -f "${destination}" ]]; then
    current_hash="$(sha256sum "${destination}")"
    current_hash="${current_hash%% *}"
  fi
  if [[ "${current_hash}" == "${expected_hash}" ]]; then
    return
  fi
  local temporary
  temporary="$(mktemp "${downloads}/download.XXXXXX")"
  curl --fail --location --silent --show-error "${url}" --output "${temporary}"
  echo "${expected_hash}  ${temporary}" | sha256sum --check --status
  mv -f "${temporary}" "${destination}"
}

hugin_archive="${downloads}/hugin-${HUGIN_VERSION}.tar.xz"
vigra_archive="${downloads}/vigra-${VIGRA_VERSION}.tar.xz"
fetch_verified "${HUGIN_URL}" "${HUGIN_SHA256}" "${hugin_archive}"
fetch_verified "${VIGRA_URL}" "${VIGRA_SHA256}" "${vigra_archive}"

if [[ -n "${EXPORT_SOURCES_DIR}" ]]; then
  EXPORT_SOURCES_DIR="$(mkdir -p "${EXPORT_SOURCES_DIR}" && cd "${EXPORT_SOURCES_DIR}" && pwd)"
  install -m 0644 "${hugin_archive}" \
    "${EXPORT_SOURCES_DIR}/hugin_${HUGIN_VERSION}+dfsg.orig.tar.xz"
  install -m 0644 "${vigra_archive}" \
    "${EXPORT_SOURCES_DIR}/libvigraimpex_${VIGRA_VERSION}+dfsg.orig.tar.xz"
  echo "${HUGIN_SHA256}  ${EXPORT_SOURCES_DIR}/hugin_${HUGIN_VERSION}+dfsg.orig.tar.xz" \
    | sha256sum --check --status
  echo "${VIGRA_SHA256}  ${EXPORT_SOURCES_DIR}/libvigraimpex_${VIGRA_VERSION}+dfsg.orig.tar.xz" \
    | sha256sum --check --status
fi
if [[ -z "${OUTPUT_DIR}" ]]; then
  echo "[hugin-tools] Verified sources exported to ${EXPORT_SOURCES_DIR}"
  exit 0
fi
if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "ERROR: Jetson Hugin tools must be built natively on aarch64." >&2
  exit 1
fi
for command_name in cmake file ninja readelf tar; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "ERROR: required Jetson Hugin build command not found: ${command_name}" >&2
    exit 1
  fi
done
OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"
if cache_is_valid; then
  echo "[hugin-tools] Reusing verified ${CACHE_FORMAT} cache."
  copy_cached_artifacts
  exit 0
fi

build_root="$(mktemp -d "${CACHE_DIR}/build.XXXXXX")"
cleanup() {
  local status=$?
  set +e
  if [[ "${build_root}" == "${CACHE_DIR}"/build.* ]]; then
    rm -rf -- "${build_root}"
  fi
  return "${status}"
}
trap cleanup EXIT

source_dir="${build_root}/source"
prefix="${build_root}/prefix"
vigra_build="${build_root}/build-vigra"
hugin_build="${build_root}/build-hugin"
artifacts="${build_root}/artifacts"
mkdir -p "${source_dir}" "${prefix}" "${vigra_build}" "${hugin_build}" \
  "${artifacts}/bin" "${artifacts}/lib" \
  "${artifacts}/licenses/hugin" "${artifacts}/licenses/vigra"
tar -xf "${vigra_archive}" -C "${source_dir}"
tar -xf "${hugin_archive}" -C "${source_dir}"

vigra_source="${source_dir}/vigra-Version-1-11-1"
hugin_source="${source_dir}/hugin-2022.0.0"
# Debian's DFSG source archive omits copyrighted test images while its old
# CMake file still configures those tests unconditionally. They are not part
# of the library or tools, so exclude the test/doc subdirectories here.
sed -i '/ADD_SUBDIRECTORY(test)/d; /ADD_SUBDIRECTORY(docsrc)/d' "${vigra_source}/CMakeLists.txt"

echo "[hugin-tools] Building pinned VIGRA ${VIGRA_VERSION}..."
cmake -S "${vigra_source}" -B "${vigra_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${prefix}" \
  -DWITH_VIGRANUMPY=OFF \
  -DWITH_HDF5=OFF \
  -DWITH_OPENEXR=ON \
  -DVIGRA_STATIC_LIB=OFF \
  -DAUTOEXEC_TESTS=OFF \
  -DAUTOBUILD_TESTS=OFF
cmake --build "${vigra_build}" --target install --parallel "$(nproc)"

echo "[hugin-tools] Building pinned Hugin ${HUGIN_VERSION} CLI tools..."
cmake -S "${hugin_source}" -B "${hugin_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${prefix}" \
  -DCMAKE_PREFIX_PATH="${prefix}" \
  -DVIGRA_INCLUDE_DIR="${prefix}/include" \
  -DVIGRA_LIBRARIES="${prefix}/lib/libvigraimpex.so" \
  -DHUGIN_SHARED=ON \
  -DBUILD_HSI=OFF \
  -DDISABLE_DPKG=ON
cmake --build "${hugin_build}" --target autooptimiser pano_modify nona --parallel "$(nproc)"

install -m 0755 "${hugin_build}/src/tools/autooptimiser" "${artifacts}/bin/autooptimiser"
install -m 0755 "${hugin_build}/src/tools/pano_modify" "${artifacts}/bin/pano_modify"
install -m 0755 "${hugin_build}/src/tools/nona" "${artifacts}/bin/nona"
install -m 0644 "${hugin_build}/src/hugin_base/libhuginbase.so.0.0" \
  "${artifacts}/lib/libhuginbase.so.0.0"
install -m 0644 "${prefix}/lib/libvigraimpex.so.11.1.11.1" \
  "${artifacts}/lib/libvigraimpex.so.11.1.11.1"
ln -s libvigraimpex.so.11.1.11.1 "${artifacts}/lib/libvigraimpex.so.11"
install -m 0644 "${hugin_source}/COPYING.txt" "${artifacts}/licenses/hugin/COPYING.txt"
install -m 0644 "${vigra_source}/LICENSE.txt" "${artifacts}/licenses/vigra/LICENSE.txt"

for executable in "${artifacts}/bin/autooptimiser" "${artifacts}/bin/pano_modify" "${artifacts}/bin/nona"; do
  if [[ "$(file -Lb "${executable}")" != *aarch64* ]]; then
    echo "ERROR: Hugin tool is not an aarch64 ELF: ${executable}" >&2
    exit 1
  fi
done
if [[ "$(readelf -d "${artifacts}/lib/libhuginbase.so.0.0" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')" != \
      "libhuginbase.so.0.0" ||
      "$(readelf -d "${artifacts}/lib/libvigraimpex.so.11.1.11.1" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')" != \
      "libvigraimpex.so.11" ]]; then
  echo "ERROR: pinned Hugin/VIGRA libraries have unexpected SONAMEs." >&2
  exit 1
fi
LD_LIBRARY_PATH="${artifacts}/lib" "${artifacts}/bin/autooptimiser" --help >/dev/null
LD_LIBRARY_PATH="${artifacts}/lib" "${artifacts}/bin/pano_modify" --help >/dev/null
LD_LIBRARY_PATH="${artifacts}/lib" "${artifacts}/bin/nona" --help >/dev/null

(
  cd "${artifacts}"
  sha256sum \
    bin/autooptimiser \
    bin/pano_modify \
    bin/nona \
    lib/libhuginbase.so.0.0 \
    lib/libvigraimpex.so.11.1.11.1 \
    licenses/hugin/COPYING.txt \
    licenses/vigra/LICENSE.txt > SHA256SUMS
)

if [[ -e "${cached_artifacts}" ]]; then
  echo "ERROR: invalid Hugin tools cache already exists: ${cached_artifacts}" >&2
  echo "Remove that exact cache directory before retrying." >&2
  exit 1
fi
mv "${artifacts}" "${cached_artifacts}"
copy_cached_artifacts
echo "[hugin-tools] Ready: ${OUTPUT_DIR}"
