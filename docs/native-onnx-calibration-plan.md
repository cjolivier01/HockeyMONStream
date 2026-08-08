# Native ONNX calibration and Python-free runtime plan

## Objective

HMStream must not start Python in any production workflow. `hstream-ui`,
`hstream-cli`, `run.sh`, and the Debian-installed launchers must perform camera
orientation, control-point generation, field-mask generation, scoreboard
selection, and pretrained-asset setup with native code.

The learned calibration models will be exported to ONNX once, outside the
runtime path, and executed from C++. Python remains permitted only in explicit
developer export tools and source-checkout parity tests; developer parity may
skip when HockeyMOM is absent, while release parity is mandatory.

The finished work must:

- preserve the current HockeyMOM calibration results within documented
  tolerances;
- cleanly configure and stitch `tv-12-1-r2` through the same path used by the UI;
- retain the current one-pass and explicit two-stage workflows;
- build and test on x86_64, non-Jetson ARM64/SBSA, and Jetson;
- produce installable Ubuntu 24.04 and Ubuntu 26.04 Debian packages;
- remove the bundled HockeyMOM/PyTorch/MMCV/MMDetection/LightGlue Python runtime
  and Python package dependencies from the Debian package; and
- keep large ONNX files out of Git, downloading content-addressed, checksummed,
  retention-controlled model assets instead.

Release qualification is stricter than developer convenience: parity, native
model execution, complete target builds, and CPU smoke tests are non-waivable.
Only explicitly named hardware-dependent rendering/encoding tests may be marked
unavailable, with the host and evidence recorded.

## Runtime boundary

The no-Python requirement is an invariant, not merely a preferred path.

Allowed:

- native HMStream binaries and libraries;
- ONNX inference from C++;
- Hugin tools (`pto_gen`, `autooptimiser`, `nona`) and a native blender
  (`enblend` or `multiblend`);
- `ffmpeg`/`ffprobe`, a browser, and other non-Python system executables already
  used by the product;
- Python under an explicitly named export or parity-test target in a source
  checkout.

Forbidden in production:

- resolving or executing `python`, `python3`, `hmorientation`,
  `hmcreate_control_points`, `hmfind_ice_rink`, or `hmscoreboard`;
- importing or packaging hmlib, PyTorch, MMCV, MMDetection, MMEngine, Kornia, or
  LightGlue Python modules;
- silently falling back to Python if a native model or native dependency is
  missing.

Missing native assets must produce a direct error naming the missing asset and
its configured source. This is preferable to crossing the runtime boundary.

## Existing Python launch sites to replace

| Area | Current behavior | Native replacement |
| --- | --- | --- |
| Camera orientation | `ConfigureStitching.cpp` runs `hmorientation` | Native video discovery, first-frame extraction, rink inference, edge-occupancy classification, and YAML update |
| Control points | Runs `hmcreate_control_points` using SuperPoint and LightGlue | Native preprocessing and ONNX execution, native match filtering, and native PTO control-point insertion |
| Field mask | Runs `hmfind_ice_rink` using Mask2Former | Reuse the native rink model and write mask/centroid/bbox metadata from C++ |
| Scoreboard | Runs the Python `hmscoreboard` web selector | Small native HTTP selector that serves the stitched PNG and writes the selected polygon |
| UI asset setup | `HmStreamWindow` starts `setup_pretrained_assets.py` | Native asset manager library called in process |
| Local launcher | `run.sh` starts `setup_pretrained_assets.py` | Native `hmstream-assets` binary, or asset setup performed by `hstream-cli` before pipeline construction |
| Installed launcher | Packaged `run.sh` starts Python and exports Python paths | Native asset setup with no Python environment manipulation |
| Debian payload | Bundles hmlib and a large Python ML runtime | Bundle only HMStream native code, private native dependencies, config, and non-engine assets |

Developer-only Python scripts elsewhere in `scripts/` are out of the runtime
path and may remain, but runtime tests will prove that launchers and binaries do
not invoke them.

## Proposed native architecture

### 1. ONNX execution layer

Add a small `src/libs/onnx/` library with:

- RAII ownership for environment, session, input/output names, tensors, and
  errors;
- validation of model input/output names, element types, ranks, fixed
  dimensions, and supported dynamic dimensions before inference;
- checked shape/size multiplication and actionable status messages;
- thread-safe session reuse and bounded memory behavior;
- deterministic CPU execution as the portability baseline;
- an optional accelerated execution provider only after it passes the same
  parity suite on the applicable host; and
- an injectable session interface so preprocessing and postprocessing unit
  tests do not need large models.

Backend selection is a gated engineering spike, not an assumption:

1. Export representative rink, SuperPoint, and LightGlue graphs.
2. Test them with ONNX Runtime, TensorRT, and OpenCV DNN on the exact operators
   and shapes used here.
3. Execute all three graphs on local x86_64, an identified ARM64/SBSA host, and
   `stubby` (Jetson), collecting parity, latency, peak RSS, and peak GPU memory.
4. Select the smallest backend/provider combination that meets the correctness
   and platform requirements. Different models or platforms may use different
   providers behind the same C++ session interface.

The expected default is ONNX Runtime because it can provide a consistent ONNX
interface on x86_64 and aarch64. TensorRT, CUDA, OpenCV DNN, or their execution
providers may be selected per graph/platform only if the exact exported graph
parses cleanly and parity is retained. A CPU provider remains available so
model engines are not tied to a GPU architecture. The final backend/provider
matrix and measured results will be recorded in this document before the
production port.

Initial operational budgets for the feasibility gate are:

- x86_64 and SBSA: no single graph inference over 60 seconds, complete
  orientation under 180 seconds, complete feature matching under 120 seconds,
  peak host RSS under 12 GiB, and peak device allocation under 8 GiB;
- Jetson: no single graph inference over 180 seconds, complete first-time
  calibration under 15 minutes, and peak host/device allocation under 70% of
  the memory available before calibration; and
- all platforms: no OOM, swap storm, process watchdog timeout, unsupported
  operator fallback, or silent provider fallback.

These are blocking interactive-product ceilings, not benchmark targets. They
may be tightened after comparison with the Python reference. Relaxing them
requires an explicit product decision recorded in the PR.

Prebuilt native runtime archives, if used, will be pinned by version and SHA256.
Selection keys include architecture, OS release, glibc and libstdc++ ABI,
provider, CUDA/TensorRT version, and JetPack version—not architecture alone.
Bazel will select by the target platform so Jetson cross-builds do not
accidentally link an SBSA or x86 library. Builds and installed packages verify
ELF architecture, SONAME, required symbol versions, and RPATH. Debian packages
will carry a private copy under `/opt/hmstream/lib` when the target distribution
does not provide the exact ABI; this must not alter system CUDA, NCCL, or
DeepStream packages.

### 2. Model contracts and export tooling

Before exporting or publishing any derived graph, Phase 0 must document legal
approval for the model implementation, original checkpoint, derived ONNX file,
and redistribution/modification terms, including required notices, model card,
and any applicable training-data restrictions. SuperPoint is specifically
blocked pending verification because the vendored source/weight lineage carries
separate restrictive language. If redistribution is not permitted, no derived
asset will be uploaded: the project must obtain a redistributable checkpoint or
select a licensed replacement and explicitly redefine parity/product behavior.

After that gate, add an explicit developer target such as
`make export-calibration-onnx`. It may
use a detected HockeyMOM checkout/environment, but it is never called by a
runtime binary, launcher, package post-install script, or ordinary build.

Every exported graph gets a sidecar manifest containing:

- source repository revision;
- source checkpoint URL and SHA256;
- exporter and opset versions;
- input and output names, dtypes, shapes, color order, normalization, and resize
  policy;
- postprocessing thresholds and class mapping;
- exact resize dimensions and rounding, interpolation and antialias flags,
  pixel-center/align-corners convention, padding side/value/divisor, color
  range/order, threshold comparison (`>` versus `>=`), activation, class/output
  ordering, bbox inclusivity, centroid representation, stable NMS/top-K tie
  breaking, empty-output behavior, dynamic-axis bounds, and padded-point mask
  semantics;
- numerical reference hashes/statistics for the parity fixtures; and
- output ONNX SHA256.

Exports must use inference/eval mode, deterministic settings, constant folding
where safe, and no training-only outputs. Large `.onnx` and `.engine` files use
content-addressed names, are referenced by URL plus SHA256, are protected by
the recorded retention/mirror process, and are not committed.

#### Ice-rink Mask2Former

The graph contract will expose the stable network outputs before MMDetection's
Python data-sample wrappers. C++ owns the reproducible parts of preprocessing
and postprocessing:

- BGR input from OpenCV converted exactly as the MMDetection data preprocessor
  expects;
- inference scale, aspect-preserving resize/padding, ImageNet mean/std, and
  original-size metadata;
- class score and mask-logit handling equivalent to the configured
  `MaskFormerFusionHead`;
- rink-class selection, score threshold `0.3`, mask thresholding, combination,
  resize/crop to the source dimensions, centroid, contours, and enclosing bbox.

The export-contract artifact must freeze the exact values for every checklist
item above by tracing the pinned MMDetection implementation. None may be chosen
ad hoc during the C++ port.

If MMDetection cannot export a stable raw-head graph, the exporter may wrap the
model through final combined-mask production, provided output dimensions remain
dynamic and the native code still validates and rescales the result. That
fallback must pass the same pixel-level parity criteria.

#### SuperPoint

Export the trained SuperPoint convolutional detector/descriptor weights. Keep
resize, grayscale conversion, non-maximum suppression, border removal,
thresholding, top-K selection, coordinate remapping, bilinear descriptor
sampling, and L2 normalization either in a documented graph contract or in
native code. Prefer native postprocessing where it makes behavior independently
unit-testable.

The production parameters remain those used by HockeyMOM:

- longest-side resize: 1024, with the reference implementation's exact
  interpolation, rounding, and coordinate remapping;
- maximum keypoints: 2048;
- NMS radius: 4;
- detection threshold: 0.0005;
- removed border: 4; and
- descriptor dimension: 256.

#### LightGlue

Export the SuperPoint LightGlue checkpoint with adaptive early exit and point
pruning disabled, matching the current runtime settings:

- depth confidence: `-1`;
- width confidence: `-1`; and
- filter threshold: `0.2`.

Prefer dynamic keypoint counts. If an accelerated provider requires profiles,
use a bounded profile up to 2048 and explicit masks/padding whose results match
the unpadded graph. The graph returns matched indices and scores; C++ maps them
back to original-image coordinates and performs the existing even-by-Y control
point selection.

The frozen contract also specifies the zero-keypoint/zero-match result and the
exact padded-point masking behavior; padded points may never become matches.

### 3. Native rink model and camera orientation

Create focused components instead of expanding `ConfigureStitching.cpp`:

- `RinkSegmentation`: preprocessing, inference, mask postprocessing, and
  profile statistics;
- `VideoDiscovery`: GoPro, Insta360, `camN`, `left/right`, chapter ordering, and
  relative-path handling matching `hmlib.orientation`;
- `CameraOrientation`: first-frame extraction and left/right classification by
  mask occupancy in the outer one-eighth image bands; and
- `CalibrationConfig`: typed reads/updates for private `config.yaml` with atomic
  replacement.

Orientation must reject ambiguous/duplicate results with diagnostics containing
both edge sums; it must not guess or write a half-valid config. Existing valid
left/right configuration remains a fast path.

Video decoding is a separate parity boundary. Versioned video fixtures will
freeze selected stream, first-frame/seek semantics, decoder version, pixel
format, color range/matrix, dimensions, and rotation metadata. Native decoded
BGR pixels are compared with the Python/MMCV reference before model inference;
model parity may not hide a decoder mismatch.

### 4. Native control points and Hugin orchestration

Create `FeatureMatcher` and `HuginProject` components:

1. Save the synchronized left/right frames as today.
2. Run the checksummed RaCo-ALIKED and LightGlue v3 ONNX pipeline natively.
3. Validate matched indices, finite coordinates, bounds, and a frozen minimum
   usable match count derived from Hugin validation.
4. Treat `HM_MAX_CONTROL_POINTS` as a cap. Never duplicate a correspondence.
   Use a fixed source-coordinate grid, score-ranked round-robin selection, and
   a total spatial output order so model-row permutations or a marginal match
   in one area cannot globally shift every later control point.
5. Generate the base two-image PTO with `pto_gen`, explicitly pin the two-camera
   panorama to cylindrical projection, and fit its FOV/canvas with
   `pano_modify`. Do not use `autooptimiser -s`: its heuristic can flip the
   projection after a marginal control-point change.
6. Insert escaped, locale-independent Hugin lines of the form
   `c n0 N1 x... y... X... Y... t0` at the control-point marker.
7. Continue to use checked native process execution for `autooptimiser`, `nona`,
   and the selected blender.
8. Parse the final optimizer output and require a finite control-point RMS of at
   most 50 pixels. Inspect TIFF/PNG dimensions and types before decoding,
   enforce both the configured live limit and an absolute allocation ceiling,
   then reject malformed, out-of-source, insufficient-coverage, or degenerate
   CV_16U x/y remaps and a mismatched/uniform seam before publication.

Temporary files are written under the game directory with unique names and
renamed only after success. Publication uses a locked, fsynced
`PREPARED`/`COMMITTED` journal with durable copies of the prior generation.
Malformed state or manifests fail closed; prepared transactions restore by
copy so rollback can resume after another interruption. Artifact readers retain
the same per-game lock through `ControlMasks` decoding, so they cannot observe
the individually renamed flat files as a mixed generation. A failed attempt
must not make `is_stitching_configured()` return true.

Stitch-quality parity is evaluated after Hugin optimization, not inferred from
match overlap alone. Using pinned Hugin/blender versions, compare optimized
camera/PTO parameters, homography, reprojection RMSE and maximum error, canvas
dimensions, remap displacement, seam-mask overlap, and stitched overlap-region
pixel error. Phase 0 records reference values and tolerances before the native
implementation is accepted.

### 5. Native field-mask generation

Reuse `RinkSegmentation` on the stitched frame. Persist exactly the fields
consumed by HMStream:

- `rink_mask_0.png` through `rink_mask_N.png`, one 8-bit binary mask per
  accepted instance at stitched-canvas dimensions, matching HockeyMOM's
  artifact contract;
- `rink.ice_contours_mask_count`;
- `rink.ice_contours_mask_centroid`; and
- `rink.ice_contours_combined_bbox`.

All masks are staged, re-read, and fsynced before a `PREPARED` transaction
marker becomes durable. Existing masks/config are copied into the transaction;
the root-directory renames are fsynced and a `COMMITTED` marker is made durable
before backups are removed. A reader recovers any interrupted prepared
transaction under a per-game file lock before consuming a mask. The count
matches the files, and YAML is published only after every image is readable and
has the expected dimensions.
Recovery accepts only `config.yaml` plus contiguous canonical
`rink_mask_N.png` names, rejects duplicate/unreadable manifests and unknown
state, and retains backups until an idempotent rollback finishes. Publishing a
smaller generation transactionally removes obsolete higher-index masks.
Consumers continue to union the instance masks. Mask regeneration after
rotation or canvas-size changes remains intact.

### 6. Native scoreboard selector

Keep CLI/headless and remote workflows by replacing the Python web selector
with a small native HTTP service rather than making calibration depend on Qt.
It will:

- bind to loopback by default and require explicit opt-in for a non-loopback
  address;
- serve a self-contained HTML/JavaScript page and the stitched `s.png`;
- initialize four existing points when present;
- accept exactly four bounded coordinates or an explicit "no scoreboard";
- order points upper-left, upper-right, lower-right, lower-left;
- reject malformed requests and limit request/body sizes;
- write `rink.scoreboard.perspective_polygon` atomically; and
- shut down deterministically after save/cancel or process cancellation.

Each run generates a cryptographically random, expiring capability token that
is required on every resource and mutation request. State changes are POST-only
and validate the capability plus Host/Origin where applicable. Responses use
`Cache-Control: no-store`; request bodies are bounded; tokens are invalidated
after save/cancel. Remote use documents that the tokenized URL is a bearer
credential and requires a trusted network or operator-provided TLS reverse
proxy.

"No scoreboard" retains the existing backward-compatible sentinel: four
`[0, 0]` points in `rink.scoreboard.perspective_polygon`. Completeness checks
recognize it as configured, and all scoreboard consumers must treat it as
disabled/no crop. Add old-binary compatibility and end-to-end tests proving it
does not reopen the selector or attempt a zero-area transform.

The UI will surface/open the selector URL while the pipeline waits, matching the
remote-browser capability of the current implementation. Static web assets will
be small source assets, not generated Python payloads.

### 7. Native pretrained-asset manager

Add a reusable C++ library plus a small `hmstream-assets` command. It will parse
the existing YAML schema with yaml-cpp, recursively follow enabled child
`config-file` entries, and support `path`, `file`, and `property` targets.

Download behavior:

- HTTPS only by default, redirects enabled, bounded connection/transfer
  timeouts, and a descriptive user agent;
- optional GitHub API bearer token from `GH_TOKEN`/`GITHUB_TOKEN` without
  logging it;
- same-filesystem temporary downloads, fsync, SHA256 verification, and atomic
  rename;
- a per-target lock so concurrent UI/CLI launches do not corrupt assets;
- no `sudo`, ownership changes, or writes outside configured writable roots;
- checksum verification on existing files before accepting them; and
- clear offline behavior when a valid cached asset exists.

Path and network handling also require:

- canonical allowed roots opened with directory-relative, no-follow operations;
  rejection of symlink targets and revalidation before atomic publication;
- bounded config recursion depth, visited-node count, asset count, per-asset and
  total bytes, redirect count, and elapsed time;
- cycle detection for recursive child configs;
- HTTPS certificate/hostname validation on every redirect hop;
- stripping authorization and all sensitive headers on any host change; and
- attaching a GitHub token only to the exact configured GitHub API hostname,
  never to a release-object or arbitrary redirected host.

Runtime ONNX mutation is removed. The existing YOLO artifact that currently
uses `onnx-dynamic-batch` must be replaced with an already-correct,
content-addressed dynamic-batch export, or the patching operation must be
implemented natively
with a pinned ONNX protobuf library. Publishing a correct graph is preferred.

Asset setup should be invoked once inside `hstream-cli` before pipeline
construction; `hstream-ui` can rely on the CLI status and display its output.
`run.sh` and installed wrappers will no longer choose a Python interpreter.

All calibration assets are resolved, downloaded/preseeded, checksummed, and
signature-validated before any game config or generated artifact is mutated.
Publication uses content-addressed filenames plus a signed/versioned manifest.
The PR records asset owner, retention policy, deletion protection, and an
independent mirror/recovery location. Offline-preseed and cache-migration
commands are documented and tested. GitHub release assets are not treated as
intrinsically immutable merely because their URL contains a tag.

### 8. Packaging and installation

Update `scripts/make_deb.sh` and the Ubuntu 24.04/26.04 container builds to:

- stop staging HockeyMOM and LightGlue Python source;
- stop staging Python site-packages and their private shared libraries;
- remove `python3`, NumPy, OpenCV-Python, Pillow, SciPy, tifffile, matplotlib,
  and PyYAML dependencies that are present only for calibration;
- stage the selected native ONNX runtime when it is not a viable distro
  dependency, including license and architecture validation;
- stage `hmstream-assets`, selector assets, model manifests, and config;
- retain the exact DeepStream 9.1 dependency while removing HMStream's obsolete
  private NCCL payload, pin, and downgrade behavior;
- verify ELF architecture/RPATH and ensure no packaged executable or script
  refers to Python; and
- verify install, upgrade, uninstall, and non-root first-run behavior in clean
  Ubuntu 24.04 and Ubuntu 26.04 containers.

The package does not include the large calibration ONNX files. First use
downloads the content-addressed files into the existing configured cache/data
location; an offline administrator may preseed the same checksummed paths.
Assets hosted by the private HMStream GitHub repository use the release-asset
API URL and require `GH_TOKEN` or `GITHUB_TOKEN` with repository read access.
The native asset manager never logs that token and does not forward its
authorization header to a redirect on another host. Public third-party assets
do not require a token.

Artifact production is explicit:

| Platform tuple | Required deployment artifact |
| --- | --- |
| Ubuntu 24.04 amd64 + DeepStream 9.1 | `deb-ubuntu24` amd64 Debian package |
| Ubuntu 26.04 amd64 + DeepStream 9.1 | `deb-ubuntu26` amd64 Debian package |
| ARM64/SBSA (host not currently identified) | Native opt build and runtime tree; add a native ARM64 Debian target only if its OS/DeepStream tuple has a supported package dependency |
| `stubby` JetPack 6 Jetson | Jetson native/cross build and runtime tree using the installed JetPack/DeepStream ABI; not an amd64/SBSA Debian artifact |

ARM/Jetson are not allowed to consume an amd64 package or a generic aarch64
runtime archive. If ARM packages become a deliverable, add target-OS native or
cross container builders and clean install gates before claiming them.

## Test strategy

### Always-on C++ unit tests

Add colocated C++ tests for:

- tensor shape/type validation, overflow rejection, and ONNX error propagation;
- rink preprocessing color order, normalization, resize/pad metadata, mask
  thresholding, crop/rescale, union, centroid, contour, and bbox calculations;
- orientation edge occupancy, ambiguous masks, duplicate classifications, and
  YAML serialization;
- all supported video filename/chapter layouts and malformed/non-consecutive
  chapter sets;
- SuperPoint NMS, borders, threshold/top-K, descriptor interpolation and
  normalization, and coordinate rescaling;
- LightGlue output validation, match mapping, fewer-than-limit duplicate-index
  behavior, and even-by-Y selection;
- PTO parsing/insertion, locale independence, escaping, empty/invalid matches,
  and atomic output behavior;
- scoreboard point ordering, bounds, no-scoreboard behavior, HTTP parsing and
  size limits, persistence, cancellation, and clean shutdown;
- asset YAML traversal, disabled child configs, target resolution, checksum
  verification, interrupted downloads, atomic replacement, concurrent locking,
  redirects, timeouts, and redaction; and
- no-Python runtime policy checks over source launchers, package staging, and
  executed child-process traces.

A tiny committed ONNX fixture may be used to test the inference wrapper. Large
or trained model files may not be committed.

### Python parity tests

Parity tests are source-checkout tests only. For developer runs, they first
locate the selected Python executable and require successful imports of
HockeyMOM/hmlib and the specific reference dependencies. If those imports or
the reference checkpoint are absent, the test prints a precise `SKIP` reason
and succeeds without trying to install anything. Debian/package tests never run
Python parity tests.

Release qualification also has a non-skippable parity job in a controlled
container/environment. It pins the exact HockeyMOM revision, Python dependency
lock, checkpoints, exporter, fixture bundle, CPU thread counts, seeds, and
determinism settings. `HM_REQUIRE_ONNX_PARITY=1` (or an equivalent dedicated
target) converts any missing import/checkpoint/fixture or attempted skip into a
failure. Definition-of-done parity evidence must come from this job, not from a
successful developer skip.

The checked `make qualify-native-onnx` entrypoint requires an explicit
`HM_PARITY_PYTHON`, checksummed rink config/checkpoint, and at least two
colon-separated fixture directories in `HM_ONNX_PARITY_GAME_DIRS`. Every
fixture must contain checksummed `left.png`, `right.png`, and `s.png` entries in
the committed fixture manifest. Canonical fixture paths and manifest identities
must be unique. Qualification verifies the pinned HockeyMOM, MMDetection, and
LightGlue revisions and the exact rink/matcher ONNX SHA-256 values before it
runs every fixture separately with Bazel test-result caching disabled and
requires the Hugin executables.

The canonical oracle is deterministic Python CPU output. The harness first
captures and versions CPU goldens, compares native CPU to those goldens, and
only then qualifies each accelerated provider against the native CPU result.
Python CUDA output is diagnostic and is never the sole oracle for native CPU.
ONNX graph optimization level, seeds, thread counts, denormal behavior, and
provider options are pinned.

When available, the parity harness runs native and Python reference
implementations on identical, versioned fixture inputs and compares:

- rink preprocessing tensors and raw exported outputs;
- combined rink mask intersection-over-union, centroid, and bbox;
- orientation edge sums and final left/right classification;
- SuperPoint keypoint count/coordinates/scores/descriptors;
- LightGlue match pairs/scores and final selected control points;
- generated PTO control-point coordinates; and
- final field-mask metadata.

Initial acceptance tolerances, to be tightened after observing deterministic
exports:

- orientation classification: exact;
- rink mask IoU: at least 0.99 against the Python result;
- centroid and bbox edges: at most 0.5% of the corresponding stitched-image
  dimension per axis. Both implementations use HockeyMOM's production
  `inference_scale=0.5`; on the 13,931x4,968 `tv-12-1-r2` fixture the measured
  mask IoU is 0.996398, centroid deltas are 37.3x22.9 pixels, and the maximum
  bbox-edge delta is 16 pixels;
- SuperPoint: at least 99% identical selected keypoints within 0.25 pixel and
  descriptor cosine similarity at least 0.999;
- the pinned upstream RaCo-ALIKED v3 optimized export: at least 64 stable
  one-to-one accepted spatial pairs, at least 80% native/Python spatial overlap,
  a native/Python accepted-count ratio in [0.8, 1.2], and all shared
  coordinates within 0.01 source pixel; the match
  percentage, exported keypoint indices, scores, eager rank-selected order,
  and whole-set planar homography are diagnostics because upstream explicitly
  documents provider-dependent marginal correspondences and a fisheye rink is
  not described by one homography; and
- final selected control points/PTO against the same ONNX graph through every
  supported native provider: same count and order, coordinates within 0.25
  pixel.

The comparison specification is machine-readable and defines exact tensor
shape/count equality, absolute and relative error, NaN/Inf rejection, threshold
boundary cases, stable point assignment for unordered candidates, the
denominator for every percentage, and whether coordinates are in resized,
padded, detector, or original-image space. A missing or extra output fails
before numerical tolerance is applied.

The RaCo-ALIKED exception above is based on two frozen fixtures. Eager CPU and
ONNX Runtime CPU share 86.5% of accepted spatial pairs on `tv-12-1-r2` and
80.7% on `dh-tv-12-1`; shared coordinates differ by less than 0.001 pixel and
accepted-count ratios remain within the declared bounds. The v3 publisher
likewise describes the optimized artifact as "near-parity" and reports
provider-dependent coverage. This is a model-identity/behavior gate rather
than an unsupported bit-parity claim. Mandatory qualification also generates
native and legacy-SuperPoint cylindrical Hugin mappings, requires finite
bounded relative camera pose/canvas deltas, and validates every nona mapping;
the clean timed stitch remains the release outcome gate.

Any other relaxed tolerance must be justified by a measured provider-specific
floating-point difference and must not change the final orientation or valid
stitch outcome.

### Integration and workflow tests

Run all of the following without a Python executable available on `PATH` and
with Python-related environment variables unset:

1. `hmstream-assets` against a local HTTP fixture and the real model manifest.
2. Existing configured-game one-pass path.
3. Clean unconfigured-game one-pass path.
4. Explicit `--two-stage` path.
5. Direct `pipeline-app`/`hstream-cli` display, encode, and fake-sink variants
   with short time limits where the host supports them.
6. `hstream-ui` Play path, including asset progress and native scoreboard
   selection.
7. Clean `tv-12-1-r2`, configure orientation/control points/rink mask, and run
   a timed stitched pipeline. Preserve logs, artifact dimensions, selected
   camera order, control-point count, and exit status.
8. Debian-installed CLI and UI paths in clean target-OS environments.
9. Compare optimized PTO/homography, reprojection error, canvas/remap geometry,
   seam mask, and overlap pixels with the pinned Python/Hugin reference.
10. Upgrade from the previous Debian release and then downgrade back to it,
    retaining prior packages/manifests and proving the old binary can consume
    native-generated game artifacts, including the no-scoreboard sentinel.

The final `tv-12-1-r2` run must be performed after invoking the supported
stitch-clean operation, not by manually deleting a subset of files. Before
cleaning, copy its private config and generated artifacts to a timestamped
backup so results can be compared and recovered.

## Platform build and runtime matrix

| Target | Build validation | Runtime validation |
| --- | --- | --- |
| Local x86_64 | `bazelisk build --config=opt --cpu=k8 //...` and all relevant tests | Native parity when HockeyMOM is importable; clean `tv-12-1-r2`; UI-equivalent Play and timed stitch |
| Ubuntu 24.04 amd64 | `make deb-ubuntu24` in its Docker builder | Clean container dependency/install/upgrade checks; install and timed hardware runs on `monster` and `ripper`, documenting their current driver state and the known V100/DeepStream 9.1 video-convert limitation separately from calibration |
| Ubuntu 26.04 amd64 | `make deb-ubuntu26` in its Docker builder | Clean container dependency/install/upgrade/downgrade checks; no matching hardware host is currently identified |
| ARM64/SBSA | Native build on an identified SBSA host; until then, cross-build plus an aarch64 userspace CPU smoke is useful evidence but not a substitute for the required hardware gate | Native asset/model smoke tests and timed fake-sink stitch on identified SBSA hardware; install an ARM package only if the supported artifact tuple above is added |
| Jetson | `make jetson` cross-build after sysroot sync and/or native `--config=jetson` build on `stubby` | Native ONNX smoke/parity fixture and a timed pipeline on `stubby`; document memory/provider limitations |

`//...` means all Bazel targets, not only the modified libraries. Complete
compile and native CPU model-smoke checks are required on each target and cannot
be waived. A platform-only failure blocks completion unless it is a named
hardware-dependent render/encode check that is genuinely unavailable; any such
exception includes the exact host, target, command, and log.

## Phase 0 findings and open gates

Status as of 2026-08-03, before production implementation:

- Two independent plan reviewers challenged the initial design. The plan was
  revised for all findings, then both reviewers re-reviewed it and returned
  `APPROVED` with no remaining plan-level actions.
- LightGlue code and pretrained matcher weights are Apache-2.0, but its own
  README explicitly states that the included SuperPoint inference code and
  pretrained weights use Magic Leap's different restrictive license. The
  upstream license permits only internal, noncommercial research and prohibits
  redistribution/sublicensing of code, weights, and derivatives. A public
  derived SuperPoint ONNX asset therefore cannot be part of this implementation
  without separate permission.
- ALIKED plus its matching LightGlue checkpoint is the recommended licensed
  replacement candidate: ALIKED is BSD-3-Clause and LightGlue is Apache-2.0.
  This is a model behavior change, so it requires an explicit product decision,
  a pinned Python ALIKED+LightGlue oracle, and the full downstream stitch-quality
  qualification; it cannot be called exact parity with legacy
  SuperPoint+LightGlue.
- The HockeyMOM root `LICENSE` file is empty. The ice-rink model implementation
  derives from Apache-2.0 MMDetection, but the custom checkpoint release has no
  license/model-card metadata. The checkpoint owner must confirm authority to
  export and redistribute the derived rink ONNX and document the training-data
  provenance before publication.
- Host discovery shows `monster` and `ripper` are Ubuntu 24.04 x86_64, and both
  currently report an NVIDIA driver/library mismatch through NVML. `stubby` is
  Ubuntu 22.04 aarch64 Jetson Orin. No ARM64/SBSA host is currently identified;
  cross-build/QEMU CPU smoke evidence cannot replace the SBSA hardware runtime
  gate.

Implementation status as of 2026-08-04:

- The requested technical implementation proceeds with CPU ONNX Runtime
  1.23.2 and the redistributable ALIKED plus LightGlue matcher instead of the
  restricted SuperPoint weights. This is explicitly behavioral/downstream
  stitch parity, not exact SuperPoint keypoint parity.
- The custom ice-rink ONNX asset remains in a private release. Technical tests
  may consume it, but public redistribution remains blocked until the owner
  records checkpoint and training-data provenance plus redistribution terms.
- The private-asset API path is content-addressed by SHA256 and authenticated
  through `GH_TOKEN`/`GITHUB_TOKEN`; clean offline installations must preseed
  the same checksummed cache paths.
- The source-only Python rink oracle uses HockeyMOM's production 0.5 inference
  scale, releases the matcher model before rink inference, and has a 36 GiB
  address-space guard. This prevents a full-resolution Mask2Former query-mask
  expansion from exhausting the host during opt-in parity tests.

The requested technical production implementation is present in this PR, but
public release of the rink asset remains blocked on its model-rights gate. The
missing SBSA host likewise prevents claiming the final all-platform definition
of done; neither limitation is represented as completed validation below.

## Implementation phases and gates

### Phase 0: feasibility spike and frozen contracts

- Complete and record the model/code/checkpoint/training-data provenance and
  redistribution gate before exporting or uploading any derivative.
- Produce temporary exports for all three models.
- Freeze every preprocessing/postprocessing/decoder/numerical contract item in
  the sidecar manifests and machine-readable parity specification.
- Inspect them with ONNX validation tooling.
- Run controlled Python-CPU goldens and candidate native providers.
- Execute all three graphs on local x86_64, identified SBSA hardware, and
  `stubby`; measure operator support, output parity, peak RSS/device memory, and
  latency against the stated budgets. Until an SBSA host is identified, also
  run the cross-build and an aarch64 userspace CPU smoke, without misreporting
  those as the hardware gate.
- Select dependencies by the full target tuple and verify link/load/SONAME/RPATH
  on each host.
- Capture baseline stitch-quality metrics with pinned Hugin/blender versions.
- Record the backend/version and final tensor contracts here.

Gate: do not begin the production port until redistribution is approved, all
three graphs load and execute within budget on every required platform, parity
passes, and any bounded graph rewrite is validated. If current SuperPoint
cannot legally be redistributed, stop for an explicit licensed-model/product
decision rather than silently substituting a model.

### Phase 1: native inference foundation

- Add pinned cross-architecture dependency integration.
- Implement RAII/session/tensor validation.
- Add tiny-model and failure-path unit tests.
- Implement the checksummed, bounded native asset resolver/downloader, manifest
  validation, offline preseed path, and all-asset preflight.

Gate: x86_64, ARM64, and Jetson compile; unit tests pass without trained assets;
all real trained assets can be hermetically downloaded or preseeded and
validated before a game transaction starts.

### Phase 2: rink model and orientation

- Implement and unit-test rink preprocessing/postprocessing.
- Port video discovery and YAML updates.
- Add developer-skippable and release-required Python parity modes, including
  decoded-video pixel parity.
- Validate orientation on `tv-12-1-r2` without changing its live artifacts yet.

Gate: the non-skippable controlled parity job passes exact orientation and rink
tolerances; native execution meets every target budget.

### Phase 3: SuperPoint, LightGlue, and PTO

- Implement detector/matcher native pipeline.
- Port match selection and PTO insertion.
- Add unit tests and both parity modes, including true-cap/no-duplicate and
  model-row-permutation-invariant selection behavior.
- Run Hugin generation into a temporary comparison directory.

Gate: the controlled match/PTO parity tolerances and post-optimization stitch
quality metrics pass; generated maps/canvas satisfy existing limits.

### Phase 4: field mask, scoreboard, and orchestration

- Reuse rink inference for the stitched mask.
- Implement all-instance mask persistence and the authenticated native selector,
  including the backward-compatible no-scoreboard sentinel and UI URL
  integration.
- Add a `CalibrationCoordinator`/operation context carrying cancellation,
  deadlines, staging-generation ownership, and logging through inference,
  asset downloads, HTTP selection, and Hugin subprocesses.
- Remove Python branches from `ConfigureStitching`.
- Exercise clean one-pass and explicit two-stage configuration.

Gate: no calibration source path can resolve or start Python; both workflows
complete.

### Phase 5: native assets and package reduction

- Finish CLI/UI integration of the Phase 1 asset manager and replace runtime
  dynamic ONNX mutation with pre-exported content-addressed graphs.
- Remove Python setup from UI and launchers.
- Remove Python payload/dependencies from Debian construction.
- Build and test both distro packages.

Gate: installed file/dependency audit is Python-free and clean installs work.

### Phase 6: full validation and `tv-12-1-r2`

- Run all x86_64 tests and `//...` opt build.
- Back up and clean `tv-12-1-r2` with the supported operation.
- Run the real UI-equivalent path through successful timed stitching.
- Validate ARM64/SBSA and Jetson builds and host smoke tests.
- Install/test on `ripper` and `monster`, separating platform/DeepStream issues
  from native calibration results.

Gate: required compile/CPU-smoke matrix is complete. Only named
hardware-dependent render/encode checks may be unavailable, and each limitation
is evidence-backed in the PR.

### Phase 7: publish and review

- Inspect and stage only intended files, preserving unrelated untracked files
  and the sibling HockeyMOM edit.
- Commit with an imperative scoped message, push the branch, and open or update
  a ready-for-review PR with the architecture, asset provenance, parity data,
  package-size/dependency delta, validation matrix, and known limitations.
- Run at least five review/fix cycles. Every cycle uses two reviewers in
  parallel: one high-reasoning reviewer and one xhigh-reasoning reviewer.
- For each cycle, address findings that improve correctness, safety,
  portability, tests, or maintainability; document declined suggestions with a
  concrete rationale; rerun impacted checks; then submit the updated commit to
  the next pair/round.
- Stop only after five complete cycles and the final reviewers report no
  necessary fixes or only documented non-blocking suggestions.

Maintain a review ledger in the PR. For every round it records the exact
reviewed commit SHA, two independent reports and their reasoning levels,
findings/dispositions, the resulting fix commit SHA, and rerun evidence. Fixes
are committed before the next round; duplicate, interrupted, or stalled reports
do not count. The fifth high+xhigh pair and final validation matrix must cover
the exact final pushed SHA. Any later code change requires another final-SHA
review pair, even after five rounds have already completed.

## Observability and failure behavior

All work runs through a `CalibrationCoordinator` with a shared cancellation
token, monotonic deadlines, generation/staging directory, and final commit
state. ONNX execution receives provider run-cancellation when supported;
downloads poll cancellation between bounded chunks; the selector exits its
accept loop; and Hugin subprocess groups receive SIGTERM followed by a bounded
wait and SIGKILL escalation. Only the coordinator owns staged artifacts.

Each calibration step logs:

- model manifest identity and SHA256, never secret tokens;
- backend/provider and device;
- input/original/resized/padded dimensions;
- inference duration and peak memory when available;
- rink candidate/mask statistics;
- orientation edge sums;
- detected keypoint, accepted match, and selected control-point counts;
- Hugin command exit codes and output artifact dimensions; and
- cache hit/download/verification state.

Errors retain the failing step and nested native status. Partial outputs remain
inside a versioned staging generation and are safe to retry. After validating
the complete PTO/mappings/seam/masks/YAML set, publish a versioned generation
manifest/commit sentinel last. Retain the previous complete generation until
commit succeeds. `is_stitching_configured()` accepts only a validated committed
generation (with a compatibility path for pre-generation legacy artifacts).
Cancellation from UI or a terminating pipeline must stop
inference/download/selector work without orphan processes or a config that
appears complete.

## Security, licensing, and reproducibility

- Pin source revisions, model URLs, native archives, and SHA256 values.
- Record and package third-party licenses for the selected native runtime and
  exported model code/weights.
- Do not export or publish a model until provenance and redistribution approval
  covers code, weights, derivative ONNX, notices/model card, and applicable
  training-data restrictions.
- Do not deserialize pickle checkpoints in production; production consumes ONNX
  only.
- Do not follow model/config paths outside allowed roots without an explicit
  absolute configuration.
- Never log GitHub tokens or forward them to non-GitHub hosts.
- Reject checksum mismatches and unexpected model signatures.
- Keep generated engines and large ONNX files untracked.

## Rollback and compatibility

There will be no automatic Python fallback. During development, comparison can
be performed by explicit parity targets. If native parity or a platform gate
fails, the change remains unmerged while the current release stays available.

Existing configured games must continue to run without recalibration. Newly
generated config keys and artifacts retain their existing format so rolling
back the binary does not invalidate game data. Asset schema additions must be
backward-compatible with existing inference configs.

Keep the prior package artifacts and model manifests available for downgrade
testing. Validate previous-release -> native release -> previous-release on
clean hosts/containers, including old-binary reads of a native-generated
orientation config, PTO/mappings, multi-mask field profile, and zero-polygon
no-scoreboard sentinel. The generation transaction retains the prior complete
artifact set until the new commit marker is durable.

## Definition of done

The work is complete only when all of the following are true:

- no production source, launcher, or package path starts Python;
- native orientation, control points, field mask, scoreboard, and asset setup
  are implemented with C++ unit coverage;
- developer HockeyMOM parity tests pass when its Python environment is present
  and skip cleanly otherwise, while the controlled release parity job passes
  without skips;
- the complete x86_64, ARM64/SBSA, and Jetson target builds and native CPU model
  smokes pass (an attempted but failed build is not completion);
- Ubuntu 24.04 and 26.04 packages build, install, and contain no calibration
  Python runtime;
- clean `tv-12-1-r2` setup and timed stitching succeed through the UI path;
- `ripper`, `monster`, and `stubby` findings are recorded;
- model assets are content-addressed, checksummed, retention-controlled,
  mirrored, licensed, and not committed;
- the branch is committed/pushed and the ready PR contains the validation
  evidence; and
- five independent high+xhigh review/fix cycles are recorded in the ledger, and
  the last pair reviewed the exact final pushed SHA with no unresolved necessary
  fixes.
