#!/usr/bin/env bash
# Private GStreamer 1.24.2 VA support/plugin with descriptor-probe cleanup
# and correct Main10 signaling. The installed SDK stays untouched.
set -euo pipefail
ensure=0
if [[ ${1:-} == --ensure ]]; then
  ensure=1
  shift
fi
if [[ $# != 2 || ! $2 =~ ^[1-9][0-9]*$ ]]; then
  echo "Usage: $0 [--ensure] NEW_PRIVATE_PREFIX BUILD_JOBS" >&2
  exit 2
fi
prefix=$(realpath -m -- "$1")
jobs=$2
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if (( !ensure )) && [[ -e $prefix ]]; then
  echo "Use a new private runtime prefix." >&2
  exit 2
fi
for command in curl tar sha256sum patch meson ninja pkg-config cc patchelf; do
  command -v "$command" >/dev/null
done
if [[ $(uname -s) != Linux ]]; then
  echo "This recipe requires Linux." >&2
  exit 2
fi
gst_version=$(pkg-config --modversion gstreamer-1.0)
va_version=$(pkg-config --modversion gstreamer-va-1.0)
case "$gst_version:$va_version" in
  1.24.2:1.24.2)
    source_version=1.24.2
    source_sha256=448e32787bc82b586c6cb2f81c9a8ef404fea4f77f25566fe06e597a3f59136b
    patches=(gstreamer-va-1.24.2-probe-fds.patch gstreamer-va-1.24.2-hevc-main10.patch)
    ;;
  1.28.2:1.28.2)
    # Ubuntu 26.04 packages the 1.28.2 VA implementation with core 1.28.2.
    # Descriptor cleanup is upstream; only the Main10 signal still needs fixing.
    source_version=1.28.2
    source_sha256=6467e3964828f4d7d08bfe1fbb4d76287a1c8fa76674e59e101a149c020fefd7
    patches=(gstreamer-va-1.28.2-hevc-main10.patch)
    ;;
  *)
    echo "Unsupported installed GStreamer core/VA versions: $gst_version/$va_version." >&2
    echo "Supported Ubuntu 24.04: 1.24.2/1.24.2; Ubuntu 26.04: 1.28.2/1.28.2." >&2
    exit 2
    ;;
esac
if ((ensure)) && [[ -x $prefix/run && -f $prefix/build-info.txt ]] &&
    grep -Fxq "GStreamer VA source: gst-plugins-bad $source_version" "$prefix/build-info.txt" &&
    grep -Fxq "GStreamer core: $gst_version" "$prefix/build-info.txt" &&
    grep -Fxq "GStreamer VA: $va_version" "$prefix/build-info.txt"; then
  printf 'Using existing private GStreamer VA runtime: %s\n' "$prefix"
  exit 0
fi
mkdir -p -- "$(dirname -- "$prefix")"
work=$(mktemp -d "$(dirname -- "$prefix")/.hstream-gstva-build-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
curl --fail --location --retry 3 \
  "https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-${source_version}.tar.xz" \
  --output "$work/source.tar.xz"
(cd -- "$work" && echo "$source_sha256  source.tar.xz" | sha256sum --check)
mkdir -- "$work/source"
tar -xf "$work/source.tar.xz" -C "$work/source" --strip-components=1
for patch_file in "${patches[@]}"; do
  patch --directory="$work/source" --strip=1 --forward < "$repo/buildfiles/patches/$patch_file"
done
meson setup "$work/build" "$work/source" --buildtype=release --wrap-mode=nofallback \
  -Dauto_features=disabled -Dva=enabled -Ddrm=enabled -Dtests=disabled \
  -Dexamples=disabled -Ddoc=disabled -Dintrospection=disabled
# Build the support library and matching VA plugin against the installed core.
ninja -C "$work/build" -j "$jobs" sys/va/libgstva.so
mkdir -p "$work/result/lib" "$work/result/plugins" "$work/result/source/scripts" "$work/result/source/buildfiles/patches"
cp -a -- "$work/build/gst-libs/gst/va/"libgstva-1.0.so* "$work/result/lib/"
cp -- "$work/build/sys/va/libgstva.so" "$work/result/plugins/"
# Do not retain transient Meson build directories in the installed ELF paths.
patchelf --remove-rpath "$work/result/plugins/libgstva.so"
for library in "$work/result/lib/"libgstva-1.0.so*; do
  [[ -f $library ]] && patchelf --remove-rpath "$library"
done
cp -- "$work/source/COPYING" "$work/result/LICENSE.GStreamer"
cp -- "$work/source.tar.xz" "$work/result/source/gst-plugins-bad-${source_version}.tar.xz"
for patch_file in "${patches[@]}"; do
  cp -- "$repo/buildfiles/patches/$patch_file" "$work/result/source/buildfiles/patches/"
done
cp -- "${BASH_SOURCE[0]}" "$work/result/source/scripts/build_amd_gstreamer.sh"
cat > "$work/result/run" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail
runtime=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export LD_LIBRARY_PATH="$runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
existing_plugins=${GST_PLUGIN_PATH_1_0:-${GST_PLUGIN_PATH:-}}
export GST_PLUGIN_PATH_1_0="$runtime/plugins${existing_plugins:+:$existing_plugins}"
exec "$@"
LAUNCHER
chmod +x "$work/result/run"
{
  echo "GStreamer VA source: gst-plugins-bad $source_version"
  echo "Source SHA256: $source_sha256"
  echo "GStreamer core: $gst_version"
  echo "GStreamer VA: $va_version"
  printf 'Applied patches:'
  printf ' %s' "${patches[@]}"
  printf '\n'
  pkg-config --modversion gstreamer-1.0 gstreamer-va-1.0 libva
  sha256sum "${patches[@]/#/$repo/buildfiles/patches/}"
} > "$work/result/build-info.txt"
mv -T -- "$work/result" "$prefix"
printf 'Private VA runtime ready. Launch with: %q/run COMMAND [ARGUMENTS...]\n' "$prefix"
