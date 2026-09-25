# Repeating player analytics measurements

Use completed private game artifacts, prepared engines and an otherwise idle GPU.
Prepare models, launch caches and calibration before measuring. Keep the input
media, source offsets, duration, sink, canvas, detector engine, inference cadence
and power settings identical across comparisons. Copy executable, custom plugins,
dependency libraries and configuration into separate master/candidate runtime
directories; ordinary copies keep later builds from changing the benchmark.
Check `ldd` under each runtime's library path and retain content hashes.

`scripts/benchmark_player_analytics.py` executes argument arrays directly without
a shell. Its JSON specification contains `cases` and a list of file `artifacts` to
hash before/after the suite. Each case supplies its name, executable arguments,
working directory, optional environment and validation expectations. Include the
same explicitly selected detector engine in each configuration; matching config
names alone do not establish matching cached engine bytes.

For example, replace these paths with private fixture/runtime paths:

```json
{
  "artifacts": [
    "/bench/master/bin/hstream-cli",
    "/bench/candidate/bin/hstream-cli",
    "/bench/detector.engine",
    "/bench/off.yaml",
    "/bench/all3-draw.yaml",
    "/bench/games/fixture/config.yaml"
  ],
  "cases": [
    {
      "name": "candidate-off",
      "cwd": "/bench/candidate",
      "command": [
        "/bench/candidate/bin/hstream-cli",
        "-c", "/bench/candidate/configs/ds_hockey_app_config.yaml",
        "-c", "/bench/off.yaml",
        "--game-id=fixture", "--enable-sources=URI-MULTIPLE",
        "--enable-sinks=FAKE", "-t=20"
      ],
      "env": {
        "HM_GAME_DIR": "/bench/games",
        "HM_OUTPUT_WORK_DIR": "/bench/output",
        "LD_LIBRARY_PATH": "/bench/candidate/lib"
      },
      "detector_engine": "/bench/detector.engine",
      "monitor_artifacts": ["/bench/games/fixture/config.yaml", "/bench/detector.engine"],
      "features": [],
      "expect_analytics": false,
      "expect_overlay": false,
      "required_log_patterns": ["App run successful"]
    },
    {
      "name": "all3-draw",
      "cwd": "/bench/candidate",
      "command": [
        "/bench/candidate/bin/hstream-cli",
        "-c", "/bench/candidate/configs/ds_hockey_app_config.yaml",
        "-c", "/bench/all3-draw.yaml",
        "--game-id=fixture", "--enable-sources=URI-MULTIPLE",
        "--enable-sinks=FAKE", "-t=20"
      ],
      "env": {
        "HM_GAME_DIR": "/bench/games",
        "HM_OUTPUT_WORK_DIR": "/bench/output",
        "LD_LIBRARY_PATH": "/bench/candidate/lib"
      },
      "detector_engine": "/bench/detector.engine",
      "monitor_artifacts": ["/bench/games/fixture/config.yaml", "/bench/detector.engine"],
      "features": ["pose", "jersey", "action"],
      "expect_analytics": true,
      "expect_overlay": true,
      "minimum_analytics_counters": {
        "pose-enqueues": 1, "jersey-enqueues": 1, "action-enqueues": 1,
        "pose-results": 1, "jersey-results": 1, "action-results": 1
      },
      "maximum_analytics_counters": {"maximum-frame-samples": 32},
      "maximum_overlay_counters": {"suppressed": 0, "rejected-commands": 0},
      "required_log_patterns": ["App run successful"]
    }
  ]
}
```

Expand `artifacts` to include the actual runtime closure, models/manifests and
calibration maps/sidecars. `monitor_artifacts` are checked around every case;
the complete `artifacts` list is checked around the suite. Warm up copied games
first because localization can legitimately rewrite filesystem bindings. Never
silently accept changed artifacts in measured comparisons.

Add master, pose, jersey-only, pose+action, all3 without drawing, boxes-only, ReID
and all3+draw+ReID cases using the same pattern. Turn off every optional drawing
flag in the disabled cases. Use the [configuration example](../configs/player-analytics/semantics-example.yaml)
for enabled settings, with target-local bundles and explicit ReID configuration.
Set per-case minimum counters for every enabled model; an action run must actually
classify a complete 9.9-second history. A clip longer than this warmup is necessary
but tracking/pose gaps can still prevent inference, correctly failing validation.

```sh
python3 scripts/benchmark_player_analytics.py \
  --spec /bench/spec.json --output /bench/check --validate-only
python3 scripts/benchmark_player_analytics.py \
  --spec /bench/spec.json --output /bench/results --rounds 3 --timeout 600
python3 -m unittest discover -s scripts -p benchmark_player_analytics_test.py -v
```

The result directory must be new. Case order reverses each round. Logs and JSON
retain all periodic FPS samples, warmup samples discarded (default one), run
medians, wall time including startup/shutdown, sampled process-tree RSS/NVML GPU
memory, loaded custom-library paths/hashes and inference/render counters.
Zero-FPS observations remain in both raw samples and the measured suffix; only
the requested number of leading warmup observations is excluded.
Library paths are sampled from `/proc` during playback and hashed after exit;
include every expected runtime file in the artifact manifest and check observed
paths against it. Missing samples do not prove a library was not loaded.
Process discovery supports kernels without `/proc/PID/task/TID/children`.

FPS is output throughput, not per-frame latency. Report run medians and paired
differences `100 * (candidate/reference - 1)` for each round; a ratio of aggregate
medians is a different statistic. Retain rejected runs with their failure reason.
RSS is host memory, not GPU allocation. Jetson's unified-memory system generally
lacks process NVML memory: report that field as unavailable and, if collecting
tegrastats separately, label its RAM as whole-system shared DRAM. Renderer counters
provide exact renderer-owned device allocation separately.

Run transfer profiling separately from throughput measurements:

```sh
python3 scripts/profile_player_analytics.py \
  --spec /bench/spec.json --case all3-draw --output /bench/profile --timeout 600
python3 scripts/profile_player_analytics.py \
  --database /bench/profile/all3-draw.sqlite --output /bench/reanalyzed
```

Nsight Systems is located on `PATH` or through `--nsys`. The profiler owns and
terminates its process group on timeout, retaining the raw report, SQLite and
logs. The analyzer identifies analytics streams from named reduction kernels,
requires compact D2H bounds (204 bytes/pose, 8 bytes/jersey or action, batch ≤8),
and checks reducer/copy counts. A spec-driven run additionally matches teardown
enqueue counters, overlay launch counts and artifact identity. Drawing streams
must have no D2H after their first raster kernel. The global transfer histogram
retains other pipeline transfers for attribution. Standalone SQLite analysis
defaults to requiring all three models but cannot check application logs.

See [the validation record](player-analytics-validation.md) for measured results,
hardware, limitations and the separate synthetic renderer timings.
