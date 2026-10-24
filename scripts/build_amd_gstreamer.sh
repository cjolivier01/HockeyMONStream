#!/usr/bin/env bash
# Private GStreamer 1.24.2 VA support/plugin with descriptor-probe cleanup
# and correct Main10 signaling. The installed SDK stays untouched.
set -euo pipefail
if [[ $# != 2 || ! $2 =~ ^[1-9][0-9]*$ ]]; then
  echo "Usage: $0 NEW_PRIVATE_PREFIX BUILD_JOBS" >&2
  exit 2
fi
prefix=$(realpath -m -- "$1")
jobs=$2
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -e $prefix ]]; then
  echo "Use a new private runtime prefix." >&2
  exit 2
fi
for command in curl tar sha256sum patch meson ninja pkg-config cc patchelf; do
  command -v "$command" >/dev/null
done
if [[ $(uname -s) != Linux || $(pkg-config --modversion gstreamer-1.0) != 1.24.2 ||
      $(pkg-config --modversion gstreamer-va-1.0) != 1.24.2 ]]; then
  echo "This recipe requires Linux and the GStreamer 1.24.2 core/VA development packages." >&2
  exit 2
fi
mkdir -p -- "$(dirname -- "$prefix")"
work=$(mktemp -d "$(dirname -- "$prefix")/.hstream-gstva-build-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
curl --fail --location --retry 3 \
  https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-1.24.2.tar.xz \
  --output "$work/source.tar.xz"
(cd -- "$work" && echo '448e32787bc82b586c6cb2f81c9a8ef404fea4f77f25566fe06e597a3f59136b  source.tar.xz' | sha256sum --check)
mkdir -- "$work/source"
tar -xf "$work/source.tar.xz" -C "$work/source" --strip-components=1
patch --directory="$work/source" --strip=1 --forward < "$repo/buildfiles/patches/gstreamer-va-1.24.2-probe-fds.patch"
patch --directory="$work/source" --strip=1 --forward < "$repo/buildfiles/patches/gstreamer-va-1.24.2-hevc-main10.patch"
meson setup "$work/build" "$work/source" --buildtype=release --wrap-mode=nofallback \
  -Dauto_features=disabled -Dva=enabled -Ddrm=enabled -Dtests=disabled \
  -Dexamples=disabled -Ddoc=disabled -Dintrospection=disabled
# Build the support library and matching VA plugin against the installed core.
ninja -C "$work/build" -j "$jobs" gst-libs/gst/va/libgstva-1.0.so.0.2402.0 sys/va/libgstva.so
mkdir -p "$work/result/lib" "$work/result/plugins" "$work/result/source/scripts" "$work/result/source/buildfiles/patches"
for library in libgstva-1.0.so libgstva-1.0.so.0 libgstva-1.0.so.0.2402.0; do
  cp -a -- "$work/build/gst-libs/gst/va/$library" "$work/result/lib/"
done
cp -- "$work/build/sys/va/libgstva.so" "$work/result/plugins/"
# Do not retain transient Meson build directories in the installed ELF paths.
patchelf --remove-rpath "$work/result/plugins/libgstva.so"
patchelf --remove-rpath "$work/result/lib/libgstva-1.0.so.0.2402.0"
cp -- "$work/source/COPYING" "$work/result/LICENSE.GStreamer"
cp -- "$work/source.tar.xz" "$work/result/source/gst-plugins-bad-1.24.2.tar.xz"
cp -- "$repo/buildfiles/patches/gstreamer-va-1.24.2-probe-fds.patch" "$work/result/source/buildfiles/patches/"
cp -- "$repo/buildfiles/patches/gstreamer-va-1.24.2-hevc-main10.patch" "$work/result/source/buildfiles/patches/"
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
  echo 'GStreamer VA source: gst-plugins-bad 1.24.2'
  echo 'Source SHA256: 448e32787bc82b586c6cb2f81c9a8ef404fea4f77f25566fe06e597a3f59136b'
  pkg-config --modversion gstreamer-1.0 gstreamer-va-1.0 libva
  sha256sum "$repo/buildfiles/patches/gstreamer-va-1.24.2-probe-fds.patch"
  sha256sum "$repo/buildfiles/patches/gstreamer-va-1.24.2-hevc-main10.patch"
} > "$work/result/build-info.txt"
mv -T -- "$work/result" "$prefix"
printf 'Private VA runtime ready. Launch with: %q/run COMMAND [ARGUMENTS...]\n' "$prefix"
