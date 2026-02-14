BAZELISK ?= bazelisk

CPU := $(shell uname -p)
# Some distros return "unknown" for uname -p; fall back to uname -m so Bazel
# still gets a valid --cpu value.
ifeq ($(CPU),unknown)
CPU := $(shell uname -m)
endif
ifeq ($(CPU),x86_64)
CPU := k8
endif

PERF_DEFINES :=
PERF_CUDA_COPTS :=
# glibc >= 2.38 on x86_64 conflicts with CUDA's rsqrt declarations unless this
# define enables the jetson-utils workaround.
ifeq ($(CPU),k8)
PERF_DEFINES += --define=glibc_math_rsqrt_conflict=1
PERF_CUDA_COPTS += --@rules_cuda//cuda:copts=-Xcompiler=-U_GNU_SOURCE
PERF_CUDA_COPTS += --@rules_cuda//cuda:copts=-Xcompiler=-D_POSIX_C_SOURCE=200809L
PERF_CUDA_COPTS += --@rules_cuda//cuda:copts=-Xcompiler=-D_XOPEN_SOURCE=700
PERF_CUDA_COPTS += --@rules_cuda//cuda:copts=-Xcompiler=-include
PERF_CUDA_COPTS += --@rules_cuda//cuda:copts=-Xcompiler=$(CURDIR)/buildfiles/compat/pthread_clock_compat.h
endif

all: print_targets

.PHONY: print_targets perf debug jetson pipeline_app pipeline_app_debug pipeline_app_jetson scoreboard_test test clean distclean expunge

perf:
	$(BAZELISK) build --config=opt --cpu=$(CPU) $(PERF_DEFINES) $(PERF_CUDA_COPTS) //...

debug:
	$(BAZELISK) build --config=debug //...

jetson:
	$(BAZELISK) build --config=jetson //...

pipeline_app:
	$(BAZELISK) build --config=opt --cpu=$(CPU) //src/apps/pipeline-app:pipeline-app

pipeline_app_debug:
	$(BAZELISK) build --config=debug //src/apps/pipeline-app:pipeline-app

pipeline_app_jetson:
	$(BAZELISK) build --config=jetson //src/apps/pipeline-app:pipeline-app

scoreboard_test:
	$(BAZELISK) run --config=debug //src/libs/scoreboard:scoreboard_test

test:
	$(BAZELISK) test --config=opt //...

clean:
	$(BAZELISK) clean

distclean expunge:
	$(BAZELISK) clean --expunge

print_targets:
	@printf '%s\n' \
		"Available make targets (run 'make <target>'):" \
		'' \
		'Build Outputs' \
		'-------------' \
		'perf               Builds every Bazel target with bazelisk (opt config, CPU auto-detected for x86_64 as k8); use for optimized binaries before deploying.' \
		'debug              Builds every Bazel target with bazelisk (debug config); use while iterating locally when you need symbols and asserts.' \
		'jetson             Builds every Bazel target with --config=jetson for JetPack/Jetson environments.' \
		'pipeline_app       Builds the pipeline-app binary with release flags and CPU autodetect.' \
		'pipeline_app_debug Builds the pipeline-app binary with debug flags.' \
		'pipeline_app_jetson Builds the pipeline-app binary with Jetson configuration.' \
		'' \
		'Verification' \
		'-------------' \
		'scoreboard_test    Runs //src/libs/scoreboard:scoreboard_test with debug flags; use for quick validation of the scoreboard library.' \
		'test               Runs the release-configured Bazel test suite across the repo.' \
		'' \
		'Maintenance & Cleanup' \
		'---------------------' \
		'clean              bazel clean to drop cached outputs when builds behave strangely or you switch branches.' \
		'distclean          bazel clean --expunge (also aliased as expunge); run for a fully fresh Bazel state if clean is insufficient.' \
		'expunge            Same as distclean; provided for convenience.' \
		'' \
		'Meta' \
		'----' \
		'print_targets      Shows this help text.'
