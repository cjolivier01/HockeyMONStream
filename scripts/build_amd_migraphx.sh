#!/usr/bin/env bash
# Build a private ROCm 6.4.1 MIGraphX core with isolated plugin symbols. The
# vendor GPU module and SDK remain unchanged. See docs/amd-gpu.md.
set -euo pipefail
if [[ $# != 3 || ! $3 =~ ^[1-9][0-9]*$ ]]; then
  echo "Usage: $0 ROCM_ROOT NEW_PRIVATE_PREFIX BUILD_JOBS" >&2
  exit 2
fi
sdk=$(realpath -e -- "$1")
prefix=$(realpath -m -- "$2")
jobs=$3
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -e $prefix || $prefix == "$sdk"/* || $sdk == "$prefix"/* ]]; then
  echo "Use a new private prefix outside the installed ROCm SDK." >&2
  exit 2
fi
# The core/plugin C++ ABI is version-specific. Never pair this source with a
# different installed release merely because the C API SONAME is compatible.
python3 - "$sdk/include/migraphx/version.h" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
for name, value in [('MAJOR', '2'), ('MINOR', '12'), ('PATCH', '0'), ('TWEAK', '"7a810363"')]:
    if not re.search(r'^#define MIGRAPHX_VERSION_' + name + r'\s+' + re.escape(value) + r'\s*$', text, re.M):
        raise SystemExit('This recipe requires the ROCm 6.4.1 MIGraphX 2.12.0 (7a810363) SDK')
PY
for command in curl tar sha256sum patch cmake ninja g++ gcc; do
  command -v "$command" >/dev/null
done
mkdir -p -- "$(dirname -- "$prefix")"
work=$(mktemp -d "$(dirname -- "$prefix")/.hstream-migraphx-build-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
curl --fail --location --retry 3 \
  https://codeload.github.com/ROCm/AMDMIGraphX/tar.gz/refs/tags/rocm-6.4.1 \
  --output "$work/source.tar.gz"
(cd -- "$work" && echo '25716eb8a7f73cba722cc60ba6a71fbf6459f5491a350c285cf1ec904c339095  source.tar.gz' | sha256sum --check)
mkdir -- "$work/source"
tar -xzf "$work/source.tar.gz" -C "$work/source" --strip-components=1
patch --directory="$work/source" --strip=1 --forward < "$repo/buildfiles/patches/migraphx-2.12-local-loader.patch"
# Ubuntu 24.04's msgpack package renamed its CMake target. This build-only
# compatibility alias leaves MIGraphX source and runtime contracts unchanged.
python3 - "$work/source/src/CMakeLists.txt" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
s = s.replace('find_package(msgpackc-cxx QUIET)', '''find_package(msgpack-cxx QUIET)
if(TARGET msgpack-cxx)
    add_library(msgpackc-cxx ALIAS msgpack-cxx)
endif()
find_package(msgpackc-cxx QUIET)''').replace('if(NOT msgpackc-cxx_FOUND)', 'if(NOT TARGET msgpackc-cxx)')
p.write_text(s)
PY
cmake -S "$work/source" -B "$work/build" -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v gcc)" -DCMAKE_CXX_COMPILER="$(command -v g++)" \
  -DCMAKE_PREFIX_PATH="$sdk" -DCMAKE_INSTALL_PREFIX="$work/install" \
  -DCMAKE_BUILD_TYPE=Release -DMIGRAPHX_ENABLE_GPU=OFF -DMIGRAPHX_ENABLE_PYTHON=OFF -DBUILD_TESTING=OFF
cmake --build "$work/build" --target migraphx --parallel "$jobs"
# Only the core loader changes. Keep the tested SDK GPU module, API libraries,
# code generators and headers together. The launcher supplies both library roots
# before process startup, including before any GStreamer plugin loads.
mkdir -p "$work/result/lib"
cp -a "$work/build/lib/"libmigraphx.so* "$work/result/lib/"
cp -- "$work/source/LICENSE" "$work/result/LICENSE.MIGraphX"
{
  echo '#!/usr/bin/env bash'
  echo 'set -euo pipefail'
  printf 'sdk=%q\n' "$sdk"
  echo 'runtime=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)'
  echo 'export LD_LIBRARY_PATH="$runtime/lib:$sdk/lib/migraphx/lib:$sdk/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"'
  echo 'exec "$@"'
} > "$work/result/run"
chmod +x "$work/result/run"
{
  echo 'MIGraphX source: ROCm 6.4.1, 2.12.0, 7a810363'
  echo 'Source SHA256: 25716eb8a7f73cba722cc60ba6a71fbf6459f5491a350c285cf1ec904c339095'
  printf 'SDK: %s\n' "$sdk"
  sha256sum "$repo/buildfiles/patches/migraphx-2.12-local-loader.patch"
} > "$work/result/build-info.txt"
mv -T -- "$work/result" "$prefix"
printf 'Private runtime ready. Launch with: %q/run COMMAND [ARGUMENTS...]\n' "$prefix"
