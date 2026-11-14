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
