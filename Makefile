TOPDIR := $(shell pwd)
BAZEL ?= bazelisk
JETSON_SYSROOT ?= /opt/jetson-sysroot
HOST_ARCH := $(shell uname -m)

all: print_targets

.PHONY: all print_targets perf debug test clean distclean expunge x86_64 jetson gstdebug \
	pipeline-app run-pipeline-app video-player run-video-player

perf:
	$(BAZEL) build --config=opt //...

debug:
	$(BAZEL) build --config=debug //...

gstdebug:
	$(BAZEL) build --config=gstdebug //...

x86_64:
	$(BAZEL) build --config=opt --cpu=k8 //...

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
	$(BAZEL) test --config=opt //...

pipeline-app:
	$(BAZEL) build --config=opt //src/apps/pipeline-app:pipeline-app

run-pipeline-app: pipeline-app
	bazel-bin/src/apps/pipeline-app/pipeline-app \
		-c configs/ds_hockey_configure_stitching.yaml \
		-c configs/ds_hockey_app_config.yaml \
		--enable-sources=URI-MULTIPLE \
		--enable-sinks=RENDER \
		--options=pipeline.hmaudio.enable=1

video-player:
	$(BAZEL) build --config=opt //src/apps/video-player:video-player

run-video-player: video-player
	bazel-bin/src/apps/video-player/video-player --help

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
		'perf           Build everything with --config=opt.' \
		'debug          Build everything with --config=debug.' \
		'gstdebug       Build debug with extra GStreamer debug defines (--config=gstdebug).' \
		'x86_64         Build optimized for x86_64 (--cpu=k8).' \
		'jetson         Build optimized for Jetson (--config=jetson, needs $(JETSON_SYSROOT)).' \
		'' \
		'Apps' \
		'----' \
		'pipeline-app   Build //src/apps/pipeline-app:pipeline-app.' \
		'run-pipeline-app  Run pipeline-app with the canonical hockey config (RENDER sink).' \
		'video-player   Build //src/apps/video-player:video-player.' \
		'run-video-player  Run video-player --help (smoke check).' \
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
