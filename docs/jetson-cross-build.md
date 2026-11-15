## Jetson cross-build from x86_64

Use this flow when you want `make jetson` to emit aarch64 binaries without building directly on the Jetson device.

1. Install the cross toolchain (once):
   ```bash
   sudo apt-get install -y \
     crossbuild-essential-arm64 gcc-13-aarch64-linux-gnu g++-13-aarch64-linux-gnu \
     qemu-user-static
   ```
   The Bazel toolchain deliberately uses GCC 13 to match Jetson Linux 39's Ubuntu
   24.04 rootfs and libstdc++ ABI. Newer host cross compilers can either exceed
   CUDA's supported host-compiler range or leak newer glibc symbol references
   into binaries linked against the Jetson sysroot.
   The `jetson` Bazel config also explicitly selects sysroot-backed local
   repositories; ordinary host builds continue to use `/usr` even while a
   synced `/opt/jetson-sysroot` exists.
2. Install the matching x86_64 CUDA toolkit (currently CUDA 13.2) on the build host.
   The sync script uses that release for host `nvcc`, even if `/usr/local/cuda`
   points at a newer toolkit. Set `JETSON_HOST_CUDA_ROOT` for a custom location.
3. Pull a Jetson sysroot (includes glibc, DeepStream, CUDA, etc.) using SSH:
   ```bash
   JETSON_HOST=ubuntu@<jetson-ip> ./scripts/sync_jetson_sysroot.sh [/opt/jetson-sysroot]
   ```
   - The script mirrors `/lib`, `/usr/include`, `/usr/lib`, and `/opt/nvidia` into the local sysroot path (default `/opt/jetson-sysroot`).
4. Build:
   ```bash
   make jetson                     # uses /opt/jetson-sysroot by default
   JETSON_SYSROOT=/path/to/sysroot make jetson   # optional override
   ```

Notes:
- Bazel uses the registered toolchain at `//toolchains/jetson` with the `//platforms:jetson` platform. `make jetson` already passes the right flags.
- If the sysroot path is empty or missing headers, `make jetson` will fail fast with a hint to re-run the sync script.
- When running directly on a Jetson device, `make jetson` will auto-symlink `/opt/jetson-sysroot` to `/`, so you can build natively without pulling a sysroot.

## Optional Qt applications (native and cross builds)

`make jetson` and `--config=jetson` skip the Qt targets. To include `hstream-ui`
and the Qt-based web/experiment helpers, install Qt 6 development packages on
the Jetson:

```bash
sudo apt-get install qt6-base-dev qt6-qpa-plugins qt6-image-formats-plugins
```

For cross builds, rerun `scripts/sync_jetson_sysroot.sh` after installing the
packages. Qt headers and libraries must come from the target sysroot; installing
Qt on the x86_64 host alone is insufficient. `hstream-ui` does not run Qt code
generators during its build, so it does not need a host `moc` matching the target
Qt version.

On either the Jetson or the cross-build host:

```bash
make jetson-qt  # includes Qt targets in the full Jetson build

# Build just the desktop application (after preparing /opt/jetson-sysroot).
bazelisk build --config=opt --config=jetson-qt //src/apps/hstream-ui:hstream-ui
```

`jetson-qt` includes the normal `jetson` configuration and sets
`--define=jetson_qt=true`. The opt-in applies to all targets in
`src/apps/hstream-ui`, including its tests and headless helpers. Native x86_64
and ARM64/SBSA builds keep their existing Qt behavior. Cross-built binaries and
tests must run on the Jetson, with the matching Qt runtime installed.
The platform plugins provide desktop/offscreen rendering, and the image plugins
provide formats such as WebP for highlight cards. Media tests also need an
FFmpeg/ffprobe build with `lavfi` and `libx264`; a minimal NVIDIA build may omit
these. A separate test installation can be selected through `PATH` without
replacing the device's FFmpeg. The source-camera fixture tests use
`HSTREAM_TEST_FFMPEG=/absolute/path/to/ffmpeg` for this override.

On an X11 desktop (`QT_QPA_PLATFORM=xcb`), Program, Stitched and camera previews
render inside the Qt window on Orin. Preview conversion uses the GPU and
CUDA-device RGBA buffers for CUDA/OpenGL presentation; it does not download
video frames to the CPU. Run against the Jetson's local display rather than an
SSH-forwarded X server. Other Qt platforms retain the separate-window path.

High-bit stitching uses `dsxvideoconvert` for its RGB10A2 input conversion on
Jetson. NVIDIA's converter omits that format from its Jetson caps, although the
GPU transform supports it. Packed 10-bit inputs stay in CUDA-device storage
because CUDA cannot map those packed EGL images on Orin.

To build or deploy a Jetson package with the Qt applications, use the separate
packaging opt-in:

```bash
make deb-jetson JETSON_DEB_HOST=stubby JETSON_QT=1
make deploy NODES=stubby JETSON_QT=1
```

These commands require a committed, clean local checkout and build its source
snapshot on the Jetson.
The package builder installs the Qt development packages when needed and
includes `hstream-ui`, the web/experiment helpers, their runtime dependencies,
and the desktop launcher. After deployment, run `hstream-ui` from the Jetson's
desktop session. Omitting `JETSON_QT=1` keeps Jetson packages headless; installing
Qt on the device alone does not opt in. The package records `X-HStream-Qt` as
`0` or `1`, and deployment checks that it matches the requested option.

## Deployment

`make perf deploy NODES=stubby` runs two independent targets: `perf` builds for
this host, then `deploy` detects stubby and builds an immutable package natively
on it. `NODES` does not make `perf` cross-compile, and deployment does not use the
local sysroot. Use `make deploy NODES=stubby` to deploy without the host build,
or `make jetson` to validate the cross-build separately.

The Jetson baseline is Ubuntu 24.04 / Jetson Linux 39, DeepStream 9.1 and CUDA
13.2. After reflashing or upgrading the device, install its development packages
(including CUDA Toolkit and OpenCV), then resync the sysroot before cross-building.
The old Ubuntu 22.04 / DeepStream 7.1 sysroot is incompatible with this baseline.

Jetson packages now depend on Ubuntu’s arm64 `hugin-tools`, `enblend` and
`gstreamer1.0-libav` packages, as desktop packages do. The Ubuntu 22.04 workaround
that bundled source-built Hugin/VIGRA is no longer used.

Jetson's paused camera decoder is flushed at the render pad and restarted
before a seek. This releases hardware decoder preroll without CPU video copies;
the existing HEVC recovery still repeats the exact requested seek. Shared paced
playback preserves an empty video epoch when audio completes its preroll flush
after the new video segment. Repeated cooperative Stop signals allow GPU cleanup
to finish under the process guard's existing deadline.
