TOPDIR := $(shell pwd)
BAZEL ?= bazelisk
BUILD_CONFIG ?= opt
JETSON_SYSROOT ?= /opt/jetson-sysroot
HOST_ARCH := $(shell uname -m)
IS_JETSON_HOST := $(shell \
	if [ -f /etc/nv_tegra_release ]; then \
		echo 1; \
	elif [ -r /proc/device-tree/compatible ] && tr '\0' '\n' < /proc/device-tree/compatible | grep -qi tegra; then \
		echo 1; \
	else \
		echo 0; \
	fi)

HOST_PLATFORM_FLAGS :=
ifeq ($(HOST_ARCH),aarch64)
ifeq ($(IS_JETSON_HOST),0)
HOST_PLATFORM_FLAGS := --config=arm64
endif
endif
HOST_CUDA_FLAGS := $(shell scripts/bazel_cuda_host_config.sh 2>/dev/null)
TARGET_UBUNTU ?= $(shell . /etc/os-release 2>/dev/null && printf '%s' "$$VERSION_ID")
DEB_OUTPUT_DIR ?= $(TOPDIR)/dist/ubuntu$(TARGET_UBUNTU)
PACKAGE_VERSION ?=
JETSON_DEB_HOST ?= stubby
JETSON_DEB_OUTPUT_DIR ?= $(TOPDIR)/dist/jetson

all: print_targets

.PHONY: all print_targets perf debug test clean distclean expunge x86_64 arm64 jetson gstdebug \
	hstream-cli run-hstream-cli hstream-ui run-hstream-ui pipeline-app run-pipeline-app \
	hstream-assets video-player run-video-player yolo-custom-lib hstream-gst-plugins qualify-native-onnx \
	deb deb-ubuntu24 deb-ubuntu26 deb-jetson wsl-deb publish publish-dry-run delete-release

perf:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //...

debug:
	$(BAZEL) build --config=debug $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //...

gstdebug:
	$(BAZEL) build --config=gstdebug $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //...

x86_64:
	$(BAZEL) build --config=opt --cpu=k8 $(HOST_CUDA_FLAGS) //...

arm64:
	@if [ "$(HOST_ARCH)" != "aarch64" ]; then \
		echo "arm64 target is for native non-Jetson arm64/SBSA hosts (for example GB300)." >&2; \
		exit 1; \
	fi
	$(BAZEL) build --config=opt --config=arm64 //...

jetson:
	@if [ "$(HOST_ARCH)" = "aarch64" ] && [ ! -e "$(JETSON_SYSROOT)" ]; then \
		echo "Creating symlink $(JETSON_SYSROOT) -> / for native Jetson build"; \
		sudo ln -sfn / "$(JETSON_SYSROOT)"; \
	fi
	@if [ "$(HOST_ARCH)" != "aarch64" ] && [ ! -d "$(JETSON_SYSROOT)/usr/include" ]; then \
		echo "Jetson sysroot not found at $(JETSON_SYSROOT). Run JETSON_HOST=<user@jetson> scripts/sync_jetson_sysroot.sh [DEST]." >&2; \
		exit 1; \
	fi
	$(BAZEL) build --config=jetson --action_env=JETSON_SYSROOT=$(JETSON_SYSROOT) --define=JETSON_SYSROOT=$(JETSON_SYSROOT) //...

test:
	$(BAZEL) test --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //...

qualify-native-onnx:
	scripts/qualify_native_onnx.sh

pipeline-app:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //src/apps/pipeline-app:pipeline-app

hstream-cli:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //src/apps/pipeline-app:hstream-cli

hstream-assets:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //src/apps/hstream-assets:hstream-assets

hstream-ui:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //src/apps/hstream-ui:hstream-ui

yolo-custom-lib:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo

hstream-gst-plugins:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) \
		//src/gst-plugins/gst-videoprep:libnvdsgst_videoprep.so \
		//src/gst-plugins/gst-playtracker:libgstplaytracker.so \
		//src/gst-plugins/gst-fieldmask:libnvdsgst_dsfieldmask.so

run-hstream-cli: hstream-cli
	bazel-bin/src/apps/pipeline-app/hstream-cli \
		-c configs/ds_hockey_configure_stitching.yaml \
		-c configs/ds_hockey_app_config.yaml \
		--enable-sources=URI-MULTIPLE \
		--enable-sinks=RENDER \
		--options=pipeline.hmaudio.enable=1

run-hstream-ui: hstream-ui
	bazel-bin/src/apps/hstream-ui/hstream-ui

run-pipeline-app: pipeline-app
	bazel-bin/src/apps/pipeline-app/pipeline-app \
		-c configs/ds_hockey_configure_stitching.yaml \
		-c configs/ds_hockey_app_config.yaml \
		--enable-sources=URI-MULTIPLE \
		--enable-sinks=RENDER \
		--options=pipeline.hmaudio.enable=1

video-player:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) //src/apps/video-player:video-player

run-video-player: video-player
	bazel-bin/src/apps/video-player/video-player --help

deb:
	@echo "Debian package output directory: $(DEB_OUTPUT_DIR)"
	scripts/make_deb_docker.sh --target-ubuntu=$(TARGET_UBUNTU) --output-dir="$(DEB_OUTPUT_DIR)" $(if $(PACKAGE_VERSION),--version="$(PACKAGE_VERSION)",) $(if $(DEEPSTREAM_DEB),--deepstream-deb="$(DEEPSTREAM_DEB)",)

deb-ubuntu24:
	$(MAKE) deb TARGET_UBUNTU=24.04

deb-ubuntu26:
	$(MAKE) deb TARGET_UBUNTU=26.04

deb-jetson:
	@echo "Jetson Debian package output directory: $(JETSON_DEB_OUTPUT_DIR)"
	scripts/make_deb_jetson.sh --host="$(JETSON_DEB_HOST)" --output-dir="$(JETSON_DEB_OUTPUT_DIR)" $(if $(PACKAGE_VERSION),--version="$(PACKAGE_VERSION)",)

publish:
	scripts/publish_release.sh

publish-dry-run:
	scripts/publish_release.sh --dry-run

delete-release:
	@if [ -z "$(or $(RELEASE_TAG),$(VERSION))" ]; then \
		echo 'Usage: make delete-release RELEASE_TAG=v0.1.0' >&2; \
		exit 2; \
	fi
	scripts/delete_release.sh "$(or $(RELEASE_TAG),$(VERSION))"

wsl-deb: deb

clean:
	$(BAZEL) clean

distclean expunge:
	$(BAZEL) clean --expunge

print_targets:
	@printf '%s\n' \
		"Available make targets (run 'make <target>'):" \
		'' \
		'Build Outputs' \
		'-------------' \
		'perf           Build everything with --config=opt (auto-adds arm64/Blackwell host flags when needed).' \
		'debug          Build everything with --config=debug (auto-adds Blackwell host flags when needed).' \
		'gstdebug       Build debug with extra GStreamer debug defines (--config=gstdebug).' \
		'x86_64         Build optimized for x86_64 (--cpu=k8, auto-adds Blackwell host flags when needed).' \
		'arm64          Build optimized for native non-Jetson arm64/SBSA hosts (--config=arm64).' \
		'jetson         Build optimized for Jetson (--config=jetson, needs $(JETSON_SYSROOT)).' \
		'' \
		'Apps' \
		'----' \
		'hstream-cli   Build //src/apps/pipeline-app:hstream-cli.' \
		'run-hstream-cli  Run hstream-cli with the canonical hockey config (RENDER sink).' \
		'hstream-ui    Build //src/apps/hstream-ui:hstream-ui.' \
		'run-hstream-ui   Run the hstream-ui desktop control surface.' \
		'pipeline-app   Build //src/apps/pipeline-app:pipeline-app.' \
		'run-pipeline-app  Run legacy pipeline-app with the canonical hockey config (RENDER sink).' \
		'video-player   Build //src/apps/video-player:video-player.' \
		'run-video-player  Run video-player --help (smoke check).' \
		'yolo-custom-lib Build //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo.' \
		'hstream-gst-plugins Build the three HStream-owned GStreamer plugins.' \
		'qualify-native-onnx Run the non-skippable native/Python ONNX release gate.' \
		'deb            Build in Docker; write the package under dist/ubuntu<version> (override DEB_OUTPUT_DIR).' \
		'deb-ubuntu24   Build the Ubuntu 24.04 package in Docker (output under dist/ubuntu24.04).' \
		'deb-ubuntu26   Build the Ubuntu 26.04 package in Docker (output under dist/ubuntu26.04).' \
		'deb-jetson     Build the Ubuntu 22.04 arm64 package on $(JETSON_DEB_HOST) (output under dist/jetson).' \
		'wsl-deb        Alias for deb; Windows installer is a later WSL wrapper, not a .deb.' \
		'publish        Build every .deb, increment the vMAJOR.MINOR.PATCH tag, tag, and publish a GitHub release.' \
		'publish-dry-run Show the next tag and planned artifacts without building or publishing.' \
		'delete-release Delete a GitHub release, its assets, and tag (requires RELEASE_TAG=vX.Y.Z and confirmation).' \
		'' \
		'Tests' \
		'-----' \
		'test           Run Bazel tests with --config=opt.' \
		'' \
		'Maintenance & Cleanup' \
		'---------------------' \
		'clean          bazel clean.' \
		'distclean      bazel clean --expunge (also aliased as expunge).' \
		'expunge        Same as distclean.' \
		'' \
		'Meta' \
		'----' \
		'print_targets  Shows this help text.'
