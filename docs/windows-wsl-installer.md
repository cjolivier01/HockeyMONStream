# Windows WSL installer

`make windows-installer` builds a small native Windows bootstrapper on Ubuntu.
It does not embed Ubuntu, HStream, DeepStream, or pretrained assets. At install
time it:

1. requests administrator approval only when machine-level WSL prerequisites
   need to be installed, while keeping the distro and launchers in the invoking
   user's account;
2. when the Store-delivered WSL runtime is absent or older than 2.7.11, silently
   installs Microsoft's pinned WSL MSI after SHA-256 and Authenticode
   verification;
3. downloads a dated Canonical Ubuntu 24.04 AMD64 WSL root filesystem and
   verifies it against the SHA-256 digest embedded in the bootstrapper;
4. imports a dedicated per-user distribution named `HStream`;
5. downloads the versioned Ubuntu 24.04 HStream package and verifies it against
   the `SHA256SUMS` from the same GitHub Release;
6. installs the user-selected local DeepStream 9.1.0-1+resolute2 package and
   HStream with the repository's normal Debian installer; and
7. creates Start-menu shortcuts for the UI, shell, games, and output folders.

The NVIDIA DeepStream package is deliberately not embedded or uploaded by the
bootstrapper. The installer validates the selected package before installation.
When the release repository is private, the installer also requests a
fine-grained GitHub token with read-only Contents access. The token is passed
to the provisioning process through its environment, is kept off the command
line, and is not stored by the installer.

## Build on Ubuntu

Install the small cross-build toolchain once:

```bash
sudo apt-get install nsis librsvg2-bin icoutils file
```

Then build an installer for the highest local release tag:

```bash
make windows-installer
```

Or select the release explicitly:

```bash
make windows-installer WINDOWS_INSTALLER_VERSION=v0.2.0
```

The output is written under `dist/windows/`. `make publish` passes its newly
allocated version to this target and publishes the resulting `.exe` alongside
the Debian packages and `SHA256SUMS`.

## Runtime requirements

- Windows 11 with hardware virtualization and WSL 2 support.
- WSLg for the Qt UI.
- An NVIDIA Windows driver that exposes CUDA to WSL at
  `/usr/lib/wsl/lib/libcuda.so.1`.
- The local AMD64 `deepstream-9.1` package at version
  `9.1.0-1+resolute2`.

The installer never installs a Linux display driver. A small dependency marker
inside the dedicated distro tells APT that the Windows-projected WSL CUDA driver
satisfies HStream's `libcuda.so.1` dependency.

The installer refuses to reuse or unregister an existing WSL distribution named
`HStream` unless it contains the ownership marker written at import time. This
prevents an unrelated distro with the same name from being modified or deleted.

Uninstall removes the Windows launcher by default. Removing the dedicated WSL
distribution is a separate, explicit confirmation because it permanently
deletes the distro's games, configuration, and output.
