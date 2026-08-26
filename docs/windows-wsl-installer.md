# Windows WSL installer

`make windows-installer` builds a small native Windows bootstrapper on Ubuntu.
It does not embed Ubuntu, HStream, DeepStream, or pretrained assets. At install
time it:

1. requests administrator approval only when machine-level WSL prerequisites
   need to be installed, while keeping the distro and launchers in the invoking
   user's account; the installer hash-verifies the exact helper bytes before
   executing them elevated, and stages the signed WSL MSI in a random
   Administrators/SYSTEM-only directory;
2. when WSL is absent or older than 2.0, silently installs Microsoft's pinned,
   verified WSL runtime; newer installed WSL 2 releases are accepted instead of
   forcing an unnecessary machine-level upgrade;
3. downloads a dated Canonical Ubuntu 24.04 AMD64 WSL root filesystem and
   verifies it against the SHA-256 digest embedded in the bootstrapper;
4. imports a dedicated per-user distribution named `HStream`;
5. before the first APT network operation, synchronizes the current user's and
   machine's valid Windows trusted roots into that distribution, excluding any
   certificates explicitly present in either Windows Disallowed store, so
   corporate TLS inspection works without disabling certificate verification;
6. downloads the versioned Ubuntu 24.04 HStream package and verifies it against
   the `SHA256SUMS` from the same GitHub Release;
7. installs the user-selected local `deepstream-9.1` package and HStream with
   the repository's normal Debian installer; and
8. creates an `HStream Tools` Start-menu folder with shortcuts for the UI,
   shell, games, and output folders. This is separate from WSLg's generated
   `HStream` app folder, which WSL periodically reconciles.

The installer and UI shortcut resolve the invoking Windows user's Videos known
folder and configure it as the dedicated Linux user's standard XDG Videos
directory. Browse therefore opens in the real Windows Videos folder, including
local redirected or OneDrive-backed locations, instead of `/home/hstream`.
The Existing game list and HStream-owned configuration remain under the managed
Linux game root by default. This keeps Linux locking and symlink behavior while
allowing source videos to stay on the Windows filesystem. An explicit
`HM_GAME_DIR` or `paths.game-root` in `~/.hstream/hstream.yaml` can still
relocate the managed game root.

The NVIDIA DeepStream package is deliberately not embedded or uploaded by the
bootstrapper. The installer validates the selected package's name, version, and
architecture before installation, but the local package is an explicitly
trusted input: Debian maintainer scripts run as root inside WSL and can access
the invoking user's mounted Windows files. Obtain it from NVIDIA and verify it
before selecting it.
Public GitHub releases download anonymously. When the release repository is
private, the installer reuses an existing GitHub CLI login only after verifying
that it can access the requested release. If authentication is still needed, it
opens GitHub's device-login page and shows and copies the one-time code. The
installer uses a pinned, checksum-verified GitHub CLI for this flow and deletes
its temporary authentication state after provisioning.

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

For a deliberately private or development release, a self-signed certificate
can be supplied as the explicit verification trust anchor. This verifies the
signature and timestamp without claiming that Windows trusts the publisher:

```bash
make publish \
  WINDOWS_SIGNING_PKCS12=/secure/hstream-self-signed.p12 \
  WINDOWS_SIGNING_PASSWORD_FILE=/secure/hstream-self-signed.password \
  WINDOWS_SIGNING_CA_FILE=/secure/hstream-self-signed.cert.pem
```

Do not set `WINDOWS_SIGNING_CA_FILE` for normal public releases. Windows shows
an unknown-publisher warning for a self-signed installer unless the certificate
is separately installed into the target machine's Trusted Root Certification
Authorities store and, where publisher policy requires it, Trusted Publishers.
Private-signing releases also prepend a prominent warning to the generated
GitHub release notes.

## Runtime requirements

- Windows 11 with hardware virtualization and WSL 2 support.
- WSLg for the Qt UI.
- An NVIDIA Windows driver that exposes CUDA to WSL at
  `/usr/lib/wsl/lib/libcuda.so.1`.
- The local AMD64 `deepstream-9.1` package at version
  `>= 9.1.0-1` and `< 9.2`; Debian revision suffixes are not required.

The installer never installs a Linux display driver. A small dependency marker
inside the dedicated distro tells APT that the Windows-projected WSL CUDA driver
satisfies HStream's `libcuda.so.1` dependency.

The installer refuses to reuse or unregister an existing WSL distribution named
`HStream` unless it can verify installer ownership. The primary proof is a
marker written inside the distro at import time. A pending import transaction
records a random, per-import WSL directory before it is created so a failed
import can be safely removed before retry. If execution stops just after WSL
registers it, that random path safely identifies the registration whose GUID
must be bound on the next run. After registration, the transaction and durable
record under `%LOCALAPPDATA%\HStream` are bound to the Lxss registration GUID as
well as its name and base path. This lets the uninstaller verify the same
registration even when the distro is damaged and cannot boot to expose its
in-distro marker, without accepting a later distro that reuses its name/path.

Uninstall removes the Windows launcher by default. Removing the dedicated WSL
distribution is a separate, explicit confirmation because it permanently
deletes the distro's games, configuration, and output. If WSL cannot unregister
the distro, uninstall stops and retains its normal retry entry and helper files.
