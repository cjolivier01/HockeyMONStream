TOPDIR := $(shell pwd)
BAZEL ?= bazelisk
BUILD_CONFIG ?= opt

# Forward `make -j N` to Bazel as --jobs=N. GNU make normalizes -jN/-j N/--jobs=N
# into a single "-jN" word in MAKEFLAGS; a bare "-j" (unlimited) maps to Bazel's
# "auto". Override directly with `make BAZEL_JOBS=16 ...`; with neither, Bazel
# keeps its own default.
MAKE_JOBS_FLAG := $(firstword $(filter -j%,$(MAKEFLAGS)))
BAZEL_JOBS ?= $(if $(MAKE_JOBS_FLAG),$(or $(patsubst -j%,%,$(MAKE_JOBS_FLAG)),auto),)
BAZEL_JOBS_FLAG := $(if $(strip $(BAZEL_JOBS)),--jobs=$(strip $(BAZEL_JOBS)),)
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
WINDOWS_INSTALLER_OUTPUT_DIR ?= $(TOPDIR)/dist/windows
WINDOWS_INSTALLER_VERSION ?=
WINDOWS_INSTALLER_REPOSITORY ?= cjolivier01/HockeyMONStream
WINDOWS_SIGNING_PKCS12 ?=
WINDOWS_SIGNING_PASSWORD_FILE ?=
WINDOWS_SIGNING_CA_FILE ?=
WINDOWS_SIGNING_TIMESTAMP_URL ?= http://timestamp.digicert.com

all: print_targets

.PHONY: amd-kernels test-amd-kernels test-amd-video test-amd-inference test-amd-tracking test-amd-driver-interop

amd-kernels:
	$(BAZEL) build --config=$(BUILD_CONFIG) --config=rocm $(BAZEL_JOBS_FLAG) //src/libs/gpu:portable_gpu_tests //src/libs/player_analytics/runtime:semantic_gpu_test

test-amd-kernels:
	$(BAZEL) test --config=$(BUILD_CONFIG) --config=rocm $(BAZEL_JOBS_FLAG) --local_test_jobs=1 --test_output=errors //src/libs/gpu:portable_gpu_tests

test-amd-video:
	$(BAZEL) test --config=$(BUILD_CONFIG) --config=rocm $(BAZEL_JOBS_FLAG) --local_test_jobs=1 --test_env=LD_LIBRARY_PATH --test_env=GST_PLUGIN_PATH_1_0 --test_output=errors //src/libs/gpu/vaapi:va_hip_frame_test //src/libs/gpu/vaapi:va_video_reader_test //src/libs/gpu/vaapi:va_video_playlist_test //src/libs/gpu/vaapi:va_stereo_video_test //src/libs/media:stereo_frame_plan_test //src/libs/media:mp4_video_track_test //src/libs/gpu/vaapi:va_probe_descriptors_test //src/libs/detection:va_detector_image_test //src/libs/program:va_program_sink_test //src/libs/program:va_program_output_test //src/libs/program:va_program_p010_output_test //src/libs/program:va_program_preview_test //src/libs/program:va_program_p010_preview_test //src/libs/program:encoded_colorimetry_test

test-amd-inference:
	$(BAZEL) test --config=$(BUILD_CONFIG) --config=rocm $(BAZEL_JOBS_FLAG) --local_test_jobs=1 --test_output=errors //src/libs/gpu/migraphx:session_test //src/libs/detection:gpu_test //src/libs/detection:postprocess_test //src/libs/detection:hip_detector_failure_test

test-amd-tracking:
	$(BAZEL) test --config=$(BUILD_CONFIG) --config=rocm $(BAZEL_JOBS_FLAG) --test_output=errors //src/libs/tracking:native_tracker_test //src/libs/tracking:program_tracking_test //src/libs/detection:rink_detection_mask_test

test-amd-driver-interop:
	$(BAZEL) test --config=$(BUILD_CONFIG) --config=rocm $(BAZEL_JOBS_FLAG) --local_test_jobs=1 --test_env=LD_LIBRARY_PATH --test_env=GST_PLUGIN_PATH_1_0 --test_output=errors //src/libs/gpu/migraphx:mesa_inference_first_test //src/libs/gpu/migraphx:mesa_graphics_first_test

.PHONY: all print_targets perf debug test clean distclean expunge x86_64 arm64 jetson gstdebug \
	hstream-job hstream-finalize hstream-calibration-export hstream-model-prepare hstream-prepared-model-validate hstream-model-infer hstream-cli run-hstream-cli hstream-ui hstream-web-job run-hstream-ui \
	hstream-assets video-player run-video-player yolo-custom-lib hstream-gst-plugins qualify-native-onnx \
	deb deb-ubuntu24 deb-ubuntu26 deb-jetson deploy undeploy wsl-deb windows-installer publish publish-dry-run delete-release \
	deepstream-deb deepstream-deb-ubuntu24 deepstream-deb-ubuntu26

perf:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //...

debug:
	$(BAZEL) build --config=debug $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //...

gstdebug:
	$(BAZEL) build --config=gstdebug $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //...

x86_64:
	$(BAZEL) build --config=opt --cpu=k8 $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //...

arm64:
	@if [ "$(HOST_ARCH)" != "aarch64" ]; then \
		echo "arm64 target is for native non-Jetson arm64/SBSA hosts (for example GB300)." >&2; \
		exit 1; \
	fi
	$(BAZEL) build --config=opt --config=arm64 $(BAZEL_JOBS_FLAG) //...

jetson:
	@if [ "$(HOST_ARCH)" = "aarch64" ] && [ ! -e "$(JETSON_SYSROOT)" ]; then \
		echo "Creating symlink $(JETSON_SYSROOT) -> / for native Jetson build"; \
		sudo ln -sfn / "$(JETSON_SYSROOT)"; \
	fi
	@if [ "$(HOST_ARCH)" != "aarch64" ] && [ ! -d "$(JETSON_SYSROOT)/usr/include" ]; then \
		echo "Jetson sysroot not found at $(JETSON_SYSROOT). Run JETSON_HOST=<user@jetson> scripts/sync_jetson_sysroot.sh [DEST]." >&2; \
		exit 1; \
	fi
	$(BAZEL) build --config=jetson --action_env=JETSON_SYSROOT=$(JETSON_SYSROOT) --define=JETSON_SYSROOT=$(JETSON_SYSROOT) $(BAZEL_JOBS_FLAG) //...

test:
	$(BAZEL) test --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //...

qualify-native-onnx:
	scripts/qualify_native_onnx.sh

hstream-cli:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-cli:hstream-cli

hstream-finalize:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-finalize:hstream-finalize

hstream-job:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-job:hstream-job

hstream-calibration-export:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-calibration-export:hstream-calibration-export

hstream-prepared-model-validate:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-prepared-model-validate:hstream-prepared-model-validate

hstream-model-prepare:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-model-prepare:hstream-model-prepare

hstream-model-infer:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-model-infer:hstream-model-infer

hstream-assets:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-assets:hstream-assets

hstream-ui:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-ui:hstream-ui

hstream-web-job:
	$(BAZEL) build --config=$(BUILD_CONFIG) $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/hstream-ui:hstream-web-job

yolo-custom-lib:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo

hstream-gst-plugins:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) \
		//src/gst-plugins/gst-dsxvideoconvert:libgstdsxvideoconvert.so \
		//src/gst-plugins/gst-videoprep:libnvdsgst_videoprep.so \
		//src/gst-plugins/gst-playtracker:libgstplaytracker.so \
		//src/gst-plugins/gst-fieldmask:libnvdsgst_dsfieldmask.so

run-hstream-cli: hstream-cli
	bazel-bin/src/apps/hstream-cli/hstream-cli \
		-c configs/ds_hockey_configure_stitching.yaml \
		-c configs/ds_hockey_app_config.yaml \
		--enable-sources=URI-MULTIPLE \
		--enable-sinks=RENDER \
		--options=pipeline.hmaudio.enable=1

run-hstream-ui: hstream-ui
	bazel-bin/src/apps/hstream-ui/hstream-ui

video-player:
	$(BAZEL) build --config=opt $(HOST_PLATFORM_FLAGS) $(HOST_CUDA_FLAGS) $(BAZEL_JOBS_FLAG) //src/apps/video-player:video-player

run-video-player: video-player
	bazel-bin/src/apps/video-player/video-player --help

deb:
	@echo "Debian package output directory: $(DEB_OUTPUT_DIR)"
	scripts/make_deb_docker.sh --target-ubuntu=$(TARGET_UBUNTU) --output-dir="$(DEB_OUTPUT_DIR)" $(if $(PACKAGE_VERSION),--version="$(PACKAGE_VERSION)",) $(if $(DEEPSTREAM_DEB),--deepstream-deb="$(DEEPSTREAM_DEB)",)

deb-ubuntu24:
	$(MAKE) deb TARGET_UBUNTU=24.04

deb-ubuntu26:
	$(MAKE) deb TARGET_UBUNTU=26.04

deepstream-deb:
	scripts/make_deepstream_deb.sh --target-ubuntu=$(TARGET_UBUNTU) --output-dir="$(DEB_OUTPUT_DIR)" $(if $(DEEPSTREAM_DEB),--deepstream-deb="$(DEEPSTREAM_DEB)",)

deepstream-deb-ubuntu24:
	$(MAKE) deepstream-deb TARGET_UBUNTU=24.04

deepstream-deb-ubuntu26:
	$(MAKE) deepstream-deb TARGET_UBUNTU=26.04

deb-jetson:
	@echo "Jetson Debian package output directory: $(JETSON_DEB_OUTPUT_DIR)"
	scripts/make_deb_jetson.sh --host="$(JETSON_DEB_HOST)" --output-dir="$(JETSON_DEB_OUTPUT_DIR)" $(if $(PACKAGE_VERSION),--version="$(PACKAGE_VERSION)",)

deploy:
	@if [ -z "$(strip $(NODES))" ]; then \
		echo 'ERROR: NODES is required. Usage: make deploy NODES=monster,stubby,mini' >&2; \
		exit 2; \
	fi
	NODES="$(NODES)" \
	PACKAGE_VERSION="$(PACKAGE_VERSION)" \
	DEEPSTREAM_DEB="$(DEEPSTREAM_DEB)" \
	DEPLOY_OUTPUT_DIR="$(if $(DEPLOY_OUTPUT_DIR),$(DEPLOY_OUTPUT_DIR),$(TOPDIR)/dist)" \
	scripts/deploy.sh

undeploy:
	@if [ -z "$(strip $(NODES))" ]; then \
		echo 'ERROR: NODES is required. Usage: make undeploy NODES=monster,stubby,mini' >&2; \
		exit 2; \
	fi
	NODES="$(NODES)" scripts/deploy.sh --undeploy

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

windows-installer:
	WINDOWS_SIGNING_PKCS12="$(WINDOWS_SIGNING_PKCS12)" \
	WINDOWS_SIGNING_PASSWORD_FILE="$(WINDOWS_SIGNING_PASSWORD_FILE)" \
	WINDOWS_SIGNING_CA_FILE="$(WINDOWS_SIGNING_CA_FILE)" \
	WINDOWS_SIGNING_TIMESTAMP_URL="$(WINDOWS_SIGNING_TIMESTAMP_URL)" \
	scripts/make_windows_installer.sh \
		--output-dir="$(WINDOWS_INSTALLER_OUTPUT_DIR)" \
		--repository="$(WINDOWS_INSTALLER_REPOSITORY)" \
		$(if $(WINDOWS_INSTALLER_VERSION),--version="$(WINDOWS_INSTALLER_VERSION)",)

clean:
	$(BAZEL) clean

distclean expunge:
	$(BAZEL) clean --expunge

print_targets:
	@printf '%s\n' \
		"Available make targets (run 'make <target>'):" \
		'' \
		"'make -j N <target>' forwards N to Bazel as --jobs=N (bare -j means --jobs=auto);" \
		'override directly with BAZEL_JOBS=N.' \
		'' \
		'Build Outputs' \
		'-------------' \
		'perf           Build everything with --config=opt (auto-adds arm64/Blackwell host flags when needed).' \
		'debug          Build everything with --config=debug (auto-adds Blackwell host flags when needed).' \
		'gstdebug       Build debug with extra GStreamer debug defines (--config=gstdebug).' \
		'x86_64         Build optimized for x86_64 (--cpu=k8, auto-adds Blackwell host flags when needed).' \
		'arm64          Build optimized for native non-Jetson arm64/SBSA hosts (--config=arm64).' \
		'jetson         Build optimized for Jetson (--config=jetson, needs $(JETSON_SYSROOT)).' \
		'amd-kernels    Build the portable GPU kernels and tests with native HIP/ROCm (see docs/amd-gpu.md).' \
		'' \
		'Apps' \
		'----' \
		'hstream-job   Build the standalone job script exporter.' \
		'hstream-finalize Build the headless publisher and cleanup supervisor.' \
		'hstream-calibration-export Build the validated calibration snapshot helper.' \
		'hstream-prepared-model-validate Validate private prepared model imports.' \
		'hstream-model-prepare Prepare exact native analytics/ReID model bundles.' \
		'hstream-model-infer Run exact prepared appearance models with native TensorRT.' \
		'hstream-cli   Build //src/apps/hstream-cli:hstream-cli.' \
		'run-hstream-cli  Run hstream-cli with the canonical hockey config (RENDER sink).' \
		'hstream-ui    Build //src/apps/hstream-ui:hstream-ui.' \
		'hstream-web-job Build the headless native web workflow adapter.' \
		'run-hstream-ui   Run the hstream-ui desktop control surface.' \
		'video-player   Build //src/apps/video-player:video-player.' \
		'run-video-player  Run video-player --help (smoke check).' \
		'yolo-custom-lib Build //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo.' \
		'hstream-gst-plugins Build the four HStream-owned GStreamer plugins.' \
		'qualify-native-onnx Run the non-skippable native/Python ONNX release gate.' \
		'deb            Build in Docker; write the package under dist/ubuntu<version> (override DEB_OUTPUT_DIR).' \
		'deb-ubuntu24   Build the Ubuntu 24.04 package in Docker (pass DEEPSTREAM_DEB=/path/to/deb if needed).' \
		'deb-ubuntu26   Build the Ubuntu 26.04 package in Docker (output under dist/ubuntu26.04).' \
		'deepstream-deb Prepare the downloaded DeepStream amd64 package for TARGET_UBUNTU (override DEB_OUTPUT_DIR).' \
		'deepstream-deb-ubuntu24 Copy the original DeepStream package to dist/ubuntu24.04.' \
		'deepstream-deb-ubuntu26 Write the dependency-relaxed DeepStream package to dist/ubuntu26.04.' \
		'deb-jetson     Build the Ubuntu 22.04 arm64 package on $(JETSON_DEB_HOST) (output under dist/jetson).' \
		'deploy         Detect, build, and force-install on comma-separated NODES (required).' \
		'undeploy       Remove HStream from comma-separated NODES without removing DeepStream.' \
		'wsl-deb        Alias for the Ubuntu .deb used by the Windows WSL bootstrapper.' \
		'windows-installer Build the small native Windows/WSL setup .exe on Ubuntu (requires NSIS).' \
		'publish        Build every .deb, increment the vMAJOR.MINOR.PATCH tag, tag, and publish a GitHub release.' \
		'publish-dry-run Show the next tag and planned artifacts without building or publishing.' \
		'delete-release Delete a GitHub release, its assets, and tag (requires RELEASE_TAG=vX.Y.Z and confirmation).' \
		'' \
		'Tests' \
		'-----' \
		'test           Run Bazel tests with --config=opt.' \
		'test-amd-kernels Run the shared pixel and preprocessing tests on AMD.' \
		'test-amd-video Run VA/HIP hardware decode, processing and encode tests on AMD.' \
		'test-amd-inference Run native MIGraphX and detector preprocessing tests on AMD.' \
		'test-amd-tracking Test the shared rink filter, native tracking and Program camera policy.' \
		'test-amd-driver-interop Test MIGraphX, VA and EGL together (requires isolated SDK loader).' \
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
