## Jetson cross-build from x86_64

Use this flow when you want `make jetson` to emit aarch64 binaries without building directly on the Jetson device.

1. Install the cross toolchain (once):
   ```bash
   sudo apt-get install -y \
     crossbuild-essential-arm64 gcc-11-aarch64-linux-gnu g++-11-aarch64-linux-gnu \
     qemu-user-static
   ```
   The Bazel toolchain deliberately uses GCC 11 to match JetPack 6's Ubuntu
   22.04 rootfs and libstdc++ ABI. Newer host cross compilers can either exceed
   CUDA's supported host-compiler range or leak newer glibc symbol references
   into binaries linked against the Jetson sysroot.
   The `jetson` Bazel config also explicitly selects sysroot-backed local
   repositories; ordinary host builds continue to use `/usr` even while a
   synced `/opt/jetson-sysroot` exists.
2. Pull a Jetson sysroot (includes glibc, DeepStream, CUDA, etc.) using SSH:
   ```bash
   JETSON_HOST=ubuntu@<jetson-ip> ./scripts/sync_jetson_sysroot.sh [/opt/jetson-sysroot]
   ```
   - The script mirrors `/lib`, `/usr/include`, `/usr/lib`, and `/opt/nvidia` into the local sysroot path (default `/opt/jetson-sysroot`).
3. Build:
   ```bash
   make jetson                     # uses /opt/jetson-sysroot by default
   JETSON_SYSROOT=/path/to/sysroot make jetson   # optional override
   ```

Notes:
- Bazel uses the registered toolchain at `//toolchains/jetson` with the `//platforms:jetson` platform. `make jetson` already passes the right flags.
- If the sysroot path is empty or missing headers, `make jetson` will fail fast with a hint to re-run the sync script.
- When running directly on a Jetson device, `make jetson` will auto-symlink `/opt/jetson-sysroot` to `/`, so you can build natively without pulling a sysroot.
