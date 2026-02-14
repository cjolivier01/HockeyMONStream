FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Enable arm64 packages for cross headers/libs.
RUN dpkg --add-architecture arm64 && \
    printf '%s\n' \
      "deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble main restricted universe multiverse" \
      "deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble-updates main restricted universe multiverse" \
      "deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble-security main restricted universe multiverse" \
      "deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble-backports main restricted universe multiverse" \
      "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports noble main restricted universe multiverse" \
      "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports noble-updates main restricted universe multiverse" \
      "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports noble-security main restricted universe multiverse" \
      "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports noble-backports main restricted universe multiverse" \
      > /etc/apt/sources.list && \
    rm -f /etc/apt/sources.list.d/* && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      curl \
      git \
      openssh-client \
      pkg-config \
      python3 \
      python3-pip \
      python3-venv \
      gcc-aarch64-linux-gnu \
      g++-aarch64-linux-gnu \
      libstdc++-13-dev:arm64 \
      libc6-dev:arm64 \
      libglib2.0-dev:arm64 \
      libgstreamer1.0-dev:arm64 \
      libgstreamer-plugins-base1.0-dev:arm64 \
      libgtk-3-dev:arm64 \
      libsoup2.4-dev:arm64 \
      libjson-glib-dev:arm64 \
      libatk1.0-dev:arm64 \
      libcairo2-dev:arm64 \
      libpango1.0-dev:arm64 \
      libgdk-pixbuf-2.0-dev:arm64 \
      libharfbuzz-dev:arm64 \
      libglew-dev:arm64 \
      libglfw3-dev:arm64 \
      freeglut3-dev:arm64 \
      libfftw3-dev:arm64 && \
    rm -rf /var/lib/apt/lists/*

# Install Bazelisk (x86_64).
RUN curl -L https://github.com/bazelbuild/bazelisk/releases/download/v1.27.0/bazelisk-linux-amd64 -o /usr/local/bin/bazelisk && \
    chmod +x /usr/local/bin/bazelisk && \
    ln -s /usr/local/bin/bazelisk /usr/local/bin/bazel

WORKDIR /workspace
