# syntax=docker/dockerfile:1
ARG TARGET_UBUNTU=24.04
FROM ubuntu:${TARGET_UBUNTU}

ARG TARGET_UBUNTU
ARG CUDA_REPOSITORY=ubuntu2404
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      gnupg \
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
      apt-file \
      autoconf \
      automake \
      binutils \
      bison \
      build-essential \
      cuda-toolkit-13-2 \
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
      libcudnn9-cuda-13 \
      libnccl2 \
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
      python3-matplotlib \
      python3-numpy \
      python3-opencv \
      python3-pil \
      python3-pip \
      python3-scipy \
      python3-tifffile \
      python3-yaml \
      qt6-base-dev \
      qt6-base-dev-tools \
      qt6-qpa-plugins \
      rsync \
      unzip \
      wget \
      zip

RUN apt-get update \
    && if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      nccl_version="$(apt-cache madison libnccl2 | awk '$3 ~ /[+]cuda12[.]/ { print $3; exit }')"; \
      test -n "${nccl_version}"; \
      apt-get install -y --no-install-recommends --allow-downgrades \
        cuda-cudart-12-6 \
        cuda-cupti-12-6 \
        cuda-nvrtc-12-6 \
        libcublas-12-6 \
        libcudnn9-cuda-12 \
        libcufft-12-6 \
        libcufile-12-6 \
        libcurand-12-6 \
        libcusolver-12-6 \
        libcusparse-12-6 \
        libcusparselt0-cuda-12 \
        "libnccl2=${nccl_version}" \
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

RUN if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      pytorch_index=https://download.pytorch.org/whl/cu126; \
      torch_version=2.13.0+cu126; \
      torchvision_version=0.28.0+cu126; \
    else \
      pytorch_index=https://download.pytorch.org/whl/cu130; \
      torch_version=2.13.0+cu130; \
      torchvision_version=0.28.0+cu130; \
    fi \
    && python3 -m pip install --break-system-packages --no-cache-dir --no-deps \
      --target /opt/hmstream-python-deps \
      --index-url "${pytorch_index}" \
      "torch==${torch_version}" \
      "torchvision==${torchvision_version}"

RUN python3 -m pip install --break-system-packages --no-cache-dir --no-deps \
      --target /opt/hmstream-python-deps \
      addict==2.4.0 \
      ffmpegio==0.11.1 \
      filelock==3.19.1 \
      fsspec==2026.4.0 \
      jinja2==3.1.6 \
      kornia==0.8.2 \
      kornia_rs==0.1.10 \
      markdown-it-py==4.0.0 \
      markupsafe==3.0.3 \
      mdurl==0.1.2 \
      mpmath==1.3.0 \
      networkx==3.6.1 \
      packaging==26.0 \
      pluggy==1.6.0 \
      pygments==2.19.2 \
      rich==14.3.3 \
      sympy==1.14.0 \
      termcolor==3.2.0 \
      tqdm==4.67.3 \
      typing-extensions==4.15.0 \
      yapf==0.43.0 \
      pycocotools==2.0.10 \
      shapely==2.1.2 \
      six==1.17.0 \
      terminaltables==3.1.10

# Ubuntu 26.04 no longer pulls setuptools in with python3-pip, while the
# pinned OpenMMLab source packages still use setuptools.build_meta.
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3-setuptools \
    && rm -rf /var/lib/apt/lists/*

RUN PYTHONPATH=/opt/hmstream-python-deps \
      python3 -m pip install --break-system-packages --no-cache-dir --no-deps --no-build-isolation \
        --target /opt/hmstream-python-deps \
        'git+https://github.com/cjolivier01/mmengine.git@8d23fcf22b5bcc233d3ea91b5203bb10220ea603'

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
    fi

RUN if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      export CUDA_HOME=/usr/local/cuda-12.6; \
      export TORCH_CUDA_ARCH_LIST='7.0;7.5;8.0;8.6;8.9;9.0'; \
    else \
      export CUDA_HOME=/usr/local/cuda-13.2; \
      export TORCH_CUDA_ARCH_LIST='7.5;8.0;8.6;8.9;9.0;10.0;12.0'; \
    fi \
    && export PATH="${CUDA_HOME}/bin:${PATH}" \
    && MMCV_WITH_OPS=1 FORCE_CUDA=1 MAX_JOBS=4 \
      PYTHONPATH=/opt/hmstream-python-deps \
      LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/libcusparseLt/12:/usr/lib/x86_64-linux-gnu/nvshmem/12:/usr/lib/x86_64-linux-gnu/nvshmem/13 \
      python3 -m pip install --break-system-packages --no-cache-dir --no-deps --no-build-isolation \
        --target /opt/hmstream-python-deps \
        'git+https://github.com/cjolivier01/mmcv.git@23cb8a88bc7e40f47e89ef0055c2c293793a0fd4' \
    && PYTHONPATH=/opt/hmstream-python-deps \
      LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/libcusparseLt/12:/usr/lib/x86_64-linux-gnu/nvshmem/12:/usr/lib/x86_64-linux-gnu/nvshmem/13 \
      python3 -m pip install --break-system-packages --no-cache-dir --no-deps --no-build-isolation \
        --target /opt/hmstream-python-deps \
        'git+https://github.com/cjolivier01/mmdetection.git@82d8a6374f7291e680f342367a520b9bf5a74538' \
    && mkdir -p /root/.ssh /home/colivier/src

# PyTorch's CUDA 12 build needs a CUDA 12 cuDNN, but NVIDIA's Debian packages
# make libcudnn9-cuda-12 conflict with DeepStream's libcudnn9-cuda-13. Bundle
# PyTorch's exact private cuDNN wheel so installing HMStream never replaces the
# host's CUDA 13 cuDNN package.
RUN if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      python3 -m pip install --break-system-packages --no-cache-dir --no-deps \
        --target /opt/hmstream-python-deps \
        nvidia-cudnn-cu12==9.10.2.21; \
      PYTHONPATH=/opt/hmstream-python-deps \
        python3 -c 'import torch; assert torch.backends.cudnn.version() == 91002'; \
    fi

RUN apt-get update \
    && if [ "${TARGET_UBUNTU}" = "24.04" ]; then \
      apt-get install -y --no-install-recommends \
        cuda-nvtx-12-6 \
        libnvjitlink-12-6; \
    fi \
    && apt-get install -y --no-install-recommends ffmpeg \
    && python3 -m pip install --break-system-packages --no-cache-dir --no-deps \
      --target /opt/hmstream-python-deps \
      platformdirs==4.9.2 \
      typeguard==4.6.0 \
    && PYTHONPATH=/opt/hmstream-python-deps \
      LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/libcusparseLt/12:/usr/lib/x86_64-linux-gnu/nvshmem/12:/usr/lib/x86_64-linux-gnu/nvshmem/13 \
      python3 -c 'import ffmpegio, inspect, kornia, mmcv, mmengine, mmdet, torch, torchvision; from mmcv.ops import MultiScaleDeformableAttention; from mmdet.apis import init_detector; from mmengine.runner.checkpoint import load_checkpoint; assert "weights_only" in inspect.signature(load_checkpoint).parameters; assert torch.version.cuda in ("12.6", "13.0")' \
    && rm -rf /var/lib/apt/lists/*

COPY build-hmstream-deb.sh /usr/local/bin/build-hmstream-deb
RUN chmod 0755 /usr/local/bin/build-hmstream-deb

ENTRYPOINT ["/usr/local/bin/build-hmstream-deb"]
