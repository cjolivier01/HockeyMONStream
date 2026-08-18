# Windows WSL installer

`make windows-installer` builds a small native Windows bootstrapper on Ubuntu.
It does not embed Ubuntu, HStream, DeepStream, or pretrained assets. At install
time it:

1. requests administrator approval only when machine-level WSL prerequisites
   need to be installed, while keeping the distro and launchers in the invoking
   user's account; the installer hash-verifies the exact helper bytes before
   executing them elevated, and stages the signed WSL MSI in a random
   Administrators/SYSTEM-only directory;
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
bootstrapper. The installer validates the selected package's name, version, and
architecture before installation, but the local package is an explicitly
trusted input: Debian maintainer scripts run as root inside WSL and can access
the invoking user's mounted Windows files. Obtain it from NVIDIA and verify it
before selecting it.
When the release repository is private, the installer also requests a
fine-grained GitHub token with read-only Contents access. The token is passed
to the provisioning process through its environment, is kept off the command
line, and is not stored by the installer.

## Build on Ubuntu

Install the small cross-build toolchain once:

```bash
sudo apt-get install nsis librsvg2-bin icoutils file osslsigncode
```

Then build an installer for the highest local release tag:

```bash
make windows-installer
```

Or select the release explicitly:

```bash
make windows-installer WINDOWS_INSTALLER_VERSION=v0.2.0
```

The output is written under `dist/windows/`. Developer builds are unsigned.
Release publication requires an Authenticode code-signing certificate and its
password in separate files:

```bash
WINDOWS_SIGNING_PKCS12=/secure/hstream-code-signing.p12 \
WINDOWS_SIGNING_PASSWORD_FILE=/secure/hstream-code-signing.password \
make publish
```

The password is read from its file instead of being placed on a command line.
The builder signs and RFC 3161 timestamps the executable with `osslsigncode`,
and the publisher independently verifies the embedded signature before staging
the `.exe` beside the Debian packages and `SHA256SUMS`. Override the timestamp
service with `WINDOWS_SIGNING_TIMESTAMP_URL` when needed.

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
`HStream` unless it can verify installer ownership. The primary proof is a
marker written inside the distro at import time. A pending import transaction
records the expected WSL registration path so a failed import can be safely
removed before retry. After registration, the transaction and the durable
record under `%LOCALAPPDATA%\HStream` are bound to the Lxss registration GUID as
well as its name and base path. This lets the uninstaller verify the same
registration even when the distro is damaged and cannot boot to expose its
in-distro marker, without accepting a later distro that reuses its name/path.

Uninstall removes the Windows launcher by default. Removing the dedicated WSL
distribution is a separate, explicit confirmation because it permanently
deletes the distro's games, configuration, and output. If WSL cannot unregister
the distro, uninstall stops and retains its normal retry entry and helper files.
