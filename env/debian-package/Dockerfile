# syntax=docker/dockerfile:1
ARG TARGET_UBUNTU=24.04
FROM ubuntu:${TARGET_UBUNTU}

ARG TARGET_UBUNTU
ARG CUDA_REPOSITORY=ubuntu2404
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
      ca-certificates \
      curl \
      gnupg

RUN find /etc/apt -type f \( -name '*.list' -o -name '*.sources' \) \
      -exec sed -i \
        -e 's|http://archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' \
        -e 's|http://security.ubuntu.com/ubuntu|https://security.ubuntu.com/ubuntu|g' {} + \
    && printf '%s\n' \
      'Acquire::Retries "5";' \
      'Acquire::http::Timeout "30";' \
      'Acquire::https::Timeout "30";' \
      >/etc/apt/apt.conf.d/80hmstream-network \
    && curl -fsSLo /tmp/cuda-keyring.deb \
      "https://developer.download.nvidia.com/compute/cuda/repos/${CUDA_REPOSITORY}/x86_64/cuda-keyring_1.1-1_all.deb" \
    && dpkg -i /tmp/cuda-keyring.deb \
    && rm -f /tmp/cuda-keyring.deb \
    && if [ "${TARGET_UBUNTU}" = "26.04" ]; then \
      curl -fsSLo /tmp/cuda-keyring-ubuntu2404.deb \
        "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb"; \
      dpkg-deb -x /tmp/cuda-keyring-ubuntu2404.deb /tmp/cuda-keyring-ubuntu2404; \
      cp /tmp/cuda-keyring-ubuntu2404/usr/share/keyrings/cuda-archive-keyring.gpg \
        /usr/share/keyrings/cuda-archive-keyring-ubuntu2404.gpg; \
      printf '%s\n' \
        'deb [signed-by=/usr/share/keyrings/cuda-archive-keyring-ubuntu2404.gpg] https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
        >/etc/apt/sources.list.d/cuda-ubuntu2404-x86_64.list; \
      rm -rf /tmp/cuda-keyring-ubuntu2404 /tmp/cuda-keyring-ubuntu2404.deb; \
    fi \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
      autoconf \
      automake \
      binutils \
      bison \
      build-essential \
      cuda-libraries-13-2 \
      dpkg-dev \
      file \
      flex \
      git \
      libabsl-dev \
      libavcodec-dev \
      libavformat-dev \
      libavutil-dev \
      libbluetooth-dev \
      libboost-all-dev \
      libcurl4-openssl-dev \
      libdbus-1-dev \
      libfftw3-dev \
      libgl-dev \
      libglew-dev \
      libglfw3-dev \
      libglib2.0-dev \
      libglu1-mesa-dev \
      libgstreamer-plugins-bad1.0-dev \
      libgstreamer-plugins-base1.0-dev \
      libgstreamer1.0-dev \
      libgstrtspserver-1.0-dev \
      libgtk-3-dev \
      libjson-glib-dev \
      libjsoncpp-dev \
      liblapack-dev \
      libmosquitto-dev \
      libopencv-dev \
      libpano13-dev \
      libpng-dev \
      libsoup2.4-dev \
      libssl-dev \
      libswresample-dev \
      libsystemd-dev \
      libtiff-dev \
      libudev-dev \
      libv4l-dev \
      libva-dev \
      libvigraimpex-dev \
      libyaml-cpp-dev \
      libx11-dev \
      ninja-build \
      openssh-client \
      patchelf \
      pkg-config \
      protobuf-compiler \
      python3 \
      qt6-base-dev \
      qt6-base-dev-tools \
      qt6-qpa-plugins \
      rsync \
      unzip \
      wget \
      zip

RUN apt-get update \
    && if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      apt-get install -y --no-install-recommends \
        cuda-cudart-12-6 \
        cuda-cupti-12-6 \
        cuda-nvrtc-12-6 \
        libcublas-12-6 \
        libcufft-12-6 \
        libcufile-12-6 \
        libcurand-12-6 \
        libcusolver-12-6 \
        libcusparse-12-6 \
        libcusparselt0-cuda-12 \
        libnvshmem3-cuda-12; \
    else \
      apt-get install -y --no-install-recommends \
        libcusparselt0 \
        libnvshmem3-cuda-13; \
    fi \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update \
    && trt_version="$(apt-cache madison libnvinfer-dev | awk '$3 ~ /^10\./ && $3 ~ /[+]cuda13[.]2$/ { print $3; exit }')" \
    && test -n "${trt_version}" \
    && printf '%s\n' \
      'Package: tensorrt* libnvinfer* libnvonnxparsers*' \
      "Pin: version ${trt_version}" \
      'Pin-Priority: 1001' \
      >/etc/apt/preferences.d/hmstream-tensorrt10 \
    && apt-get install -y --no-install-recommends \
      "libnvinfer-plugin-dev=${trt_version}" \
      "libnvonnxparsers-dev=${trt_version}" \
    && dpkg-query -W -f='${binary:Package} ${Version}\n' \
      | awk '$1 ~ /^(tensorrt|libnvinfer|libnvonnxparsers)/ && $2 !~ /^10\./ { bad = 1; print "Unexpected TensorRT package: " $0 > "/dev/stderr" } END { exit bad }' \
    && curl -fsSLo /usr/local/bin/bazelisk \
      https://github.com/bazelbuild/bazelisk/releases/download/v1.27.0/bazelisk-linux-amd64 \
    && chmod 0755 /usr/local/bin/bazelisk \
    && rm -rf /var/lib/apt/lists/*

ENV LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/libcusparseLt/12:/usr/lib/x86_64-linux-gnu/nvshmem/12:/usr/lib/x86_64-linux-gnu/nvshmem/13

RUN if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      apt-get update \
      && apt-get install -y --no-install-recommends \
        cuda-compiler-12-6 \
        libcublas-dev-12-6 \
        libnpp-dev-12-6 \
        libcusolver-dev-12-6 \
        libcusparse-dev-12-6 \
      && rm -rf /var/lib/apt/lists/*; \
    else \
      apt-get update \
      && apt-get install -y --no-install-recommends \
        cuda-compiler-13-2 \
        cuda-libraries-dev-13-2 \
      && rm -rf /var/lib/apt/lists/*; \
    fi

RUN apt-get update \
    && if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      apt-get install -y --no-install-recommends \
        cuda-nvtx-12-6 \
        libnvjitlink-12-6; \
    fi \
    && apt-get install -y --no-install-recommends enblend ffmpeg hugin-tools \
    && rm -rf /var/lib/apt/lists/*

COPY build-hmstream-deb.sh /usr/local/bin/build-hmstream-deb
RUN chmod 0755 /usr/local/bin/build-hmstream-deb

ENTRYPOINT ["/usr/local/bin/build-hmstream-deb"]
