#!/bin/bash

# Detect system architecture
ARCH=$(uname -m)
case $ARCH in
    x86_64)
        BAZEL_ARCH="amd64"
        ;;
    aarch64)
        BAZEL_ARCH="arm64"
        ;;
    *)
        echo "Unsupported architecture: $ARCH"
        exit 1
        ;;
esac

# Download the latest version of bazelisk
echo "Downloading bazelisk for $ARCH architecture..."
curl -fLo bazelisk "https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-${BAZEL_ARCH}"

INSTALL_DIR="${HOME}/.local/bin"

# Make it executable and move to a directory in PATH
chmod +x bazelisk
mv bazelisk "${INSTALL_DIR}"

# Create symbolic link for bazel
sudo ln -sf ${INSTALL_DIR}/bazelisk ${INSTALL_DIR}/bazel

echo "Bazelisk has been installed successfully in ${INSTALL_DIR}"
