TOPDIR := $(shell pwd)
BAZEL ?= bazelisk
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

all: print_targets

.PHONY: all print_targets perf debug test clean distclean expunge x86_64 arm64 jetson gstdebug \
	hmstream-cli run-hmstream-cli hmstream-ui run-hmstream-ui pipeline-app run-pipeline-app \
	video-player run-video-player yolo-custom-lib deb wsl-deb

perf:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) //...

debug:
	$(BAZEL) build --config=debug $(HOST_PLATFORM_FLAGS) //...

gstdebug:
	$(BAZEL) build --config=gstdebug $(HOST_PLATFORM_FLAGS) //...

x86_64:
	$(BAZEL) build --config=opt --cpu=k8 //...

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
	$(BAZEL) test --config=opt $(HOST_PLATFORM_FLAGS) //...

pipeline-app:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) //src/apps/pipeline-app:pipeline-app

hmstream-cli:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) //src/apps/pipeline-app:hmstream-cli

hmstream-ui:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) //src/apps/hmstream-ui:hmstream-ui

yolo-custom-lib:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo

run-hmstream-cli: hmstream-cli
	bazel-bin/src/apps/pipeline-app/hmstream-cli \
		-c configs/ds_hockey_configure_stitching.yaml \
		-c configs/ds_hockey_app_config.yaml \
		--enable-sources=URI-MULTIPLE \
		--enable-sinks=RENDER \
		--options=pipeline.hmaudio.enable=1

run-hmstream-ui: hmstream-ui
	bazel-bin/src/apps/hmstream-ui/hmstream-ui

run-pipeline-app: pipeline-app
	bazel-bin/src/apps/pipeline-app/pipeline-app \
		-c configs/ds_hockey_configure_stitching.yaml \
		-c configs/ds_hockey_app_config.yaml \
		--enable-sources=URI-MULTIPLE \
		--enable-sinks=RENDER \
		--options=pipeline.hmaudio.enable=1

video-player:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) //src/apps/video-player:video-player

run-video-player: video-player
	bazel-bin/src/apps/video-player/video-player --help

deb: hmstream-cli hmstream-ui yolo-custom-lib
	scripts/make_deb.sh

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
		'perf           Build everything with --config=opt (auto-adds --config=arm64 on non-Jetson aarch64 hosts).' \
		'debug          Build everything with --config=debug.' \
		'gstdebug       Build debug with extra GStreamer debug defines (--config=gstdebug).' \
		'x86_64         Build optimized for x86_64 (--cpu=k8).' \
		'arm64          Build optimized for native non-Jetson arm64/SBSA hosts (--config=arm64).' \
		'jetson         Build optimized for Jetson (--config=jetson, needs $(JETSON_SYSROOT)).' \
		'' \
		'Apps' \
		'----' \
		'hmstream-cli   Build //src/apps/pipeline-app:hmstream-cli.' \
		'run-hmstream-cli  Run hmstream-cli with the canonical hockey config (RENDER sink).' \
		'hmstream-ui    Build //src/apps/hmstream-ui:hmstream-ui.' \
		'run-hmstream-ui   Run the hmstream-ui desktop control surface.' \
		'pipeline-app   Build //src/apps/pipeline-app:pipeline-app.' \
		'run-pipeline-app  Run legacy pipeline-app with the canonical hockey config (RENDER sink).' \
		'video-player   Build //src/apps/video-player:video-player.' \
		'run-video-player  Run video-player --help (smoke check).' \
		'yolo-custom-lib Build //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo.' \
		'deb            Build hmstream-cli and hmstream-ui, then package everything into dist/<pkg>.deb (Linux/WSL installable).' \
		'wsl-deb        Alias for deb; Windows installer is a later WSL wrapper, not a .deb.' \
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
