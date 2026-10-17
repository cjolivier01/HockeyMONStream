# Native-canvas GPU memory

High-bit stitching uses fused RGB10A2 unpack/remap by default. Instead of allocating two half4 camera images, hm-cupano unpacks each selected source sample while remapping into FP16 blend scratch. Two 7680 × 4320 cameras save 506.25 MiB. Stitched resolution, all pyramid levels, FP16 rounding, mapping-validity alpha, and exposure/shadow grading remain unchanged; frames remain GPU-resident.

Select the previous path before startup with:

```text
--options=pipeline.hmstitcher.private-properties.fused-rgb10-remap=false
```

The default is true. Both this setting and `compact-workspace` accept true/false or 1/0 and are startup-only. Calibration uses its existing bounded capture path; the unused legacy RGBA8 calibration-staging helper has been removed.

## Automatic memory profile

The runtime selects the low-memory profile automatically when the selected CUDA device reports 8 GiB or less of total memory. GPUs above 8 GiB retain the configured pool and batch sizes, including 4090- and 5090-class devices. If the CUDA memory query fails, the runtime retains the standard settings.

Set `runtime.gpu_memory_profile` to `auto` (the implicit default), `low`, or `standard` in the machine-local user YAML or through `--options`. `low` forces the profile on any GPU; `standard` disables it on an 8 GiB GPU. Game YAML does not select a memory profile because the same recording may run on machines with different GPUs. Explicit user, game, or CLI values for an individual buffer, pool, or batch setting still take precedence over the selected profile.

The desktop UI exposes **Normal** and **Low memory** under Program Controls → Runtime. It displays an existing machine-local user profile when one is configured; otherwise its initial session choice is Low memory on the pipeline's effective GPU when that device has at most 8 GiB, and Normal on larger GPUs or when the CUDA query fails. Until the operator changes the selector, the UI leaves the profile unset so user/CLI precedence and automatic detection apply. A manual selection is passed to UI-launched runs for the rest of the application session and is never written to YAML. Saved and exported jobs therefore use the machine-local configured or automatic CLI profile.

The low-memory profile applies the qualified settings below to existing pipeline sections:

```text
pipeline.source*.num-extra-surfaces=0
pipeline.source*.low-latency-mode=1
pipeline.hmstitcher.num-output-buffers=1
pipeline.hmstitcher.properties.output-pool-extra-buffers=0
pipeline.hmstitcher.pre-converter-output-buffers=1
pipeline.hmplaycropper.num-output-buffers=1
pipeline.hmplaycropper.properties.output-pool-extra-buffers=0
pipeline.hmstitcher.private-properties.compact-workspace=true
pipeline.primary-gie.batch-size=1
pipeline.primary-gie.workspace-size=64
pipeline.primary-gie.config-file=config_infer_yolov8_hockey_fp16.yaml
```

The resolved run configuration records `runtime.resolved_gpu_memory_profile` and `runtime.detected_gpu_memory_mib` for diagnostics and telemetry provenance.

## Individual memory controls

`pipeline.hmstitcher.private-properties.compact-workspace=true` enables hm-cupano scratch reuse (default false). High-bit hmstitcher requests borrowed full-canvas blend output in both standard and compact modes. It consumes that output on the same CUDA stream through conversion and any rink-mask preparation, avoiding an extra FP16 panorama allocation, clear and copy. Standard mode retains its independent blend intermediates. Hard seams and minimized blending still return owned output, which is cached and replaced when dimensions change. Borrowed views never survive a frame or stitcher reload. No additional surfaces or concurrent frames are introduced. A completion fence waits for submitted work on every error return, including a blend allocation failure after remapping has started, before the caller can recycle input/output buffers. Jetson completes each frame before releasing its EGL mappings; x86 retains the existing successful-batch synchronization.

`pipeline.hmstitcher.pre-converter-output-buffers=1` reduces the input converter pool. Missing/zero preserves the converter default. The videoprep GObject property `output-pool-extra-buffers` controls growth beyond `num-output-buffers`; its default remains four. Set it to zero for a fixed pool. Exhausted output-pool waits preserve backpressure and respond to shutdown/seek; a low count can still stall a graph whose downstream components retain all buffers, so validate the actual sink graph.

The tested gse-16a Program/FAKE configuration resolves to:

```text
--options=pipeline.source0.num-extra-surfaces=0
--options=pipeline.source1.num-extra-surfaces=0
--options=pipeline.source0.low-latency-mode=1
--options=pipeline.source1.low-latency-mode=1
--options=pipeline.hmstitcher.num-output-buffers=1
--options=pipeline.hmstitcher.properties.output-pool-extra-buffers=0
--options=pipeline.hmstitcher.pre-converter-output-buffers=1
--options=pipeline.hmplaycropper.num-output-buffers=1
--options=pipeline.hmplaycropper.properties.output-pool-extra-buffers=0
--options=pipeline.hmstitcher.private-properties.compact-workspace=true
--options=pipeline.primary-gie.batch-size=1
--options=pipeline.primary-gie.workspace-size=64
--options=stitching.control_point_resolution=2k
```

The profile replaces the bundled default FP32 detector config with its FP16 counterpart. Bundled detector selections use a batch-one engine and a 64 MiB builder workspace setting. An explicitly selected custom detector config or engine retains its existing batch and builder contract unless those leaves are also overridden. Decoder low latency was tested on HEVC inputs with no B frames. Recordings that require frame reordering can explicitly override `low-latency-mode=0` while retaining the rest of the profile. Decoder-required capture surfaces remain allocated. The selected profile also applies to the calibration stage in the legacy two-stage flow. Prepare calibration and the rink mask before the full Program graph so segmentation and detection workspaces do not overlap. The canonical baseline remains unchanged; the automatic profile is derived after all configuration layers are loaded.

## Measured workload

A private copy of the exported `~/Videos/gse-16a/hstream-job.sh` was tested on a Quadro RTX 4000 with the existing desktop running. The native canvas was 14,259 × 4,893; the Program crop was 7678 × 4320. Detection/tracking, original grading, audio, scoreboard and telemetry remained enabled. The exported job selects FAKE, so these measurements do not qualify additional render/encode sinks.

The initial fused run completed 60.093 seconds of video, 3,603 consecutive frames and completed EOS telemetry without `CUDA_LAUNCH_BLOCKING`. Peak process VRAM was 5,482 MiB; peak total was 7,351 MiB including desktop, with 327.77 seconds elapsed. That run used the equivalent pool counts before they became explicit controls. Final-code validation is recorded in the PR.

A bounded test is not a whole-recording reliability guarantee. Qualification checks frame continuity, actual processed video duration, geometry, telemetry completion, errors and watchdogs; process exit status alone is insufficient.

## Decoder memory at chapter boundaries

Stopping a URI reader does not immediately free its NVIDIA decoder. Queued or shallow-copied video buffers keep the capture pool alive, and that pool's finalizer releases the vendor decoder context. Opening the next chapter before this finishes briefly overlaps both decoder allocations.

On an 8 GiB Quadro RTX 4000 with two 7680 × 4320 HEVC Main10 cameras, a 14,406 × 4,694 stitched canvas, render, Program 4K archive and audio, this overlap caused `cuvidCreateDecoder` to return CUDA error 2 (out of memory). The plugin then waited indefinitely for a decoder-header event. The old decoder was destroyed about 260 ms after the failed allocation. Extra decoder surfaces did not fix the full graph and increased memory pressure.

The playlist retains one reference to each original capture pool before making buffers writable. At a chapter boundary it locks the old reader in NULL, releases the selected decoder reference, and asynchronously waits until the retained pool reference is the only owner. Its final unref then completes vendor destruction synchronously before the next URI is started. Weak notifications alone cannot provide this guarantee because they precede finalization. No video copies, extra surfaces, pool sizes, detector batches or memory-profile defaults change. Both profiles use this lifetime ordering; steady-state processing is unchanged.

The borrower wait uses the pause-aware playlist stall budget. Synchronous reader retirement and pool destruction have independent 30-second guards using `HM_SHUTDOWN_WATCHDOG_SECONDS` (zero disables them). Prepared desktop process pauses defer chapter callbacks until resume, so a SIGSTOP pause cannot consume those guards. Final shutdown also locks retired readers in NULL to prevent a parent state transition from reopening them.

The qualified 15-second replay starts at 00:12:40 and crosses both first chapter boundaries. It completed 906 video frames at 3840 × 2160, retained audio, and exited normally. A vendor trace confirmed both old `cuvidDestroyDecoder` calls returned successfully before their replacement `cuvidCreateDecoder` calls. Peak process GPU memory was 5,574 MiB. A larger GPU can tolerate the old overlap, but media size matters too: the Meshy comparison used a 32 GiB RTX 5090 and 3840 × 2160 inputs, one quarter of the local input pixels.

### Regression qualification

`UriPlaylistCapturePoolDrainTest.cpp` exercises a real GStreamer capture pool, a shallow borrowed frame, metadata-only output, multiple pools and cancellation. It verifies that weak notification precedes finalization, that replacement must wait for borrowers, and that a blocked finalizer exits through the watchdog rather than hanging. Barrier and graceful-stop tests separately cover stalled peers, continuing progress, native/process pause overlap, cancellation and reader state locking.

The hardware `uri_multiple_pipeline_test` runs twelve staggered chapter cases: H.264 and HEVC Main10, zero/one/two extra surfaces, and low latency on/off. Every case checks 45 complete exact camera pairs, continuous raw audio and all six NVIDIA decoder instances' actual properties. These fixtures have no B frames. Consuming test sinks disable `enable-last-sample`: retaining the final borrowed batch until the next batch arrives would prevent chapter retirement. Production GPU preview sinks already disable this retention.

For the full recording under memory pressure, run:

```sh
python3 scripts/test_uri_playlist_memory.py \
  --game-dir /path/to/calibrated/game \
  --artifacts /path/to/new/results \
  --start-time 00:12:40 \
  --extra-surfaces 0,1,2 --low-latency default,0
```

Choose a start and duration that cross both cameras' physical chapter boundaries. Cases run sequentially with isolated inputs/configuration/output and preserved calibration timestamps. The script records the actual input codec/dimensions/frame rate and GPU, rejects incomplete handoffs, checks video packet counts and continuity, and verifies audio from the first packet through the video endpoint. `--native-render` also exercises the display; the default replaces render video with a fake sink while retaining the render audio route. `--profiles` and `--options` allow additional profile, pool and detector-batch permutations. GPU-memory measurements belong to the CLI process.

All six local 8K/Main10 cases passed with Program 4K output and audio. Zero, one and two extra surfaces peaked at 5,574, 5,766 and 5,958 MiB respectively. Output contained 906–908 consecutive frames; the small overshoot follows the existing timed-stop scheduling. No surface-count increase is needed for this workload. Archive-checker tests reject missing audio prefixes/tails, missing/duplicate/overlapping packets, invalid timestamps and failed runners; delaying the audio of a completed archive by 500 ms also failed the checker.
