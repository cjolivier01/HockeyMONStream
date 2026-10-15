# Program video output lifecycle

Desktop archive runs and exported jobs use the same native publication library.
`hstream-cli` encodes and records a run manifest; the UI or `hstream-finalize`
publishes the completed outputs and removes the run's working files. Successful
encoding alone is not successful publication.

## Responsibilities

| Operation | Owner |
| --- | --- |
| Process frames, encode video/audio, drain EOS, close containers and telemetry | `hstream-cli` |
| Record exact output paths, identities, required telemetry, and pipeline outcome | CLI/configurator and recording plugin, through `src/libs/publication` |
| Remux MP4 with faststart, publish telemetry and archive sidecars, resume interrupted publication, clean owned scratch | `src/libs/publication` |
| Supervise a headless CLI process and report final job status | `hstream-finalize` |
| Display publication progress and results after a desktop run | `hstream-ui` |
| Freeze job arguments and a unique run ID into an executable script | `hstream-job` |

## Working storage

The output root resolves from `HM_OUTPUT_WORK_DIR`, then `paths.output-root` in
configuration, then `~/hstream_output`. The per-user configuration file is
`~/.hstream/hstream.yaml`.
Exported jobs resolve the baseline, user, game and saved CLI option layers on
first admission; `HM_OUTPUT_WORK_DIR` overrides those configured roots.

Managed runs have a private directory:

```text
<output-root>/<game-id>/runs/<run-id>/
    run.yaml
    pipeline.log                 # headless runner output
    tracking_output-with-audio.hstream-managed-....mkv
    program_4k_output-with-audio.hstream-managed-....mkv
    stitched_output-with-audio.hstream-managed-....mkv
    <game-id>_telemetry-N.db
    runtime/                     # CLI runtime staging/cache for this run
    playtracker-runtime/         # effective per-run tracker configuration
```

Only enabled file sinks produce videos. The normal Program sink is `ENCODE_FILE`;
`ENCODE_PROGRAM_4K_FILE` and `ENCODE_STITCHED_FILE` select the other archives.
Rendering alone does not create an archive.

Bare configured archive filenames are placed in the run directory. Explicit
paths containing a directory are retained, including relative paths resolved
from the CLI's working directory. The manifest records the actual files so
cleanup never needs a filename glob. Custom telemetry paths are also recorded.
The CLI prints each resolved video path in `HSTREAM_OUTPUT type=archive ...`.
Managed archive reservations use a separate name from legacy working archives,
so a later run cannot rename an interrupted managed output during stale recovery.

## Exported jobs and retries

Generate a job with the UI's **Save job script** action or:

```bash
hstream-job --game-dir /path/to/games/game-42
/path/to/games/game-42/hstream-job.sh
```

The generated script executes `hstream-finalize` with a fixed run ID, game
directory, and the exact CLI command. After admitting a new encoding run, the
supervisor starts the script's runtime setup inside the private `runtime/`
directory, then executes the CLI. Busy and completed retries create no runtime
staging.
The finalizer supervises `hstream-cli --encode-job`, forwards cancellation, and
requires both a successful exit and the CLI's completed manifest before
publication. The supervisor saves the observed successful exit; an `encoded`
manifest alone cannot authorize a retry after a crash or unsuccessful exit.
Managed encoding uses the configured working container, normally
MKV; final publication produces MP4.

Before encoding, `hstream-job-<run-id>.yaml` in the game directory records the
admitted working directory and its identity. Retries follow that binding even if
the configured output root changes; missing or replaced working data is an error.
The same file becomes the publication receipt, so no extra permanent file is
needed.

Repeating the same exported script resumes publication/cleanup for the same run.
A completely successful run is verified and is not encoded or published again.
Export another script to request a new run and output generation. Failed or
interrupted encoding retains its files and log; it is not silently treated as
complete or overwritten by a retry.

Publication can also be resumed directly:

```bash
hstream-finalize --manifest /path/to/run/run.yaml
# The durable admission record/receipt can also be used:
hstream-finalize --manifest /path/to/game/hstream-job-RUN-ID.yaml
```

A receipt initially records the working-directory binding, then publication
progress, destination file identities, and cleanup state. It survives removal of
the working directory. A job exits zero only after all required outputs are
published, synchronized, and its owned scratch is
removed. A cleanup failure returns failure and can be retried without encoding.

## Finalization and faststart

Finalization uses ffmpeg with stream copy, equivalent to:

```bash
ffmpeg -hide_banner -nostdin -n -progress pipe:1 -nostats \
  -i "$working_video" \
  -map 0:v:0 -map '0:a?' -map_metadata 0 \
  -c copy -movflags +faststart \
  -f mp4 "$staging_file"
```

HEVC also receives `-tag:v hvc1`. Video and audio are not re-encoded. On Linux,
the video input is passed through an inherited `/proc/self/fd/<fd>` so ffmpeg
reads the opened source file. `HSTREAM_FFMPEG` can select the executable; the
legacy `HSTREAM_UI_FFMPEG` override is also accepted.

Faststart is added during this remux, placing the MP4 index/header metadata ahead
of the media payload for progressive playback.

The finalizer prepares video, validated SQLite telemetry snapshots, and archive
origin sidecars. It publishes each without overwriting existing files and
synchronizes files and directories. All outputs from the run share one positive
numeric generation, for example:

```text
<game-dir>/game-42-tracking_output-with-audio-1.mp4
<game-dir>/game-42-program_4k_output-with-audio-1.mp4
<game-dir>/game-42-stitched_output-with-audio-1.mp4
<game-dir>/game-42_telemetry-1.db
<game-dir>/<video-name>.mp4.hstream-archive.json
<game-dir>/hstream-job-<run-id>.yaml
```

The game root resolves from `HM_GAME_DIR`, then `paths.game-root`, then `~/Videos`.
Published videos have temporary `.hstream-pin` guards while the transaction is
unfinished, so archive discovery does not offer them prematurely. The receipt's
`state: complete` identifies successful publication and cleanup of the whole set.

## Why staging sometimes uses the game directory

When working storage and the game directory are on the same filesystem mount, the
finalizer stages under the working run's `publication/` directory. It publishes
using hard links, with no additional full-file copy.

When they are on different mounts, it uses one private hidden directory:

```text
<game-dir>/.hstream-finalize-<run-id>/
```

ffmpeg writes its final MP4 bytes directly on the destination filesystem. This
allows atomic publication with a hard link and avoids first producing an MP4 on
working storage and then copying the whole file across filesystems. The hidden
directory belongs to one run and is removed on success. An interrupted run
retains it for retry.
Separate bind mounts also require destination staging even when they expose the
same filesystem: Linux does not allow hard links across their mount boundary.
If the kernel cannot report mount identity, destination staging is the fallback.

Cleanup briefly creates private `.hstream-cleanup-<id>` directories beside the
files being removed. It atomically moves a name there and checks its identity
before deleting it, so a replacement at the original path cannot be deleted by
mistake. These moves do not copy media. The receipt records interrupted moves;
all cleanup directories disappear on success.

## Cleanup and failure behavior

No working source is deleted until every required video, database, and sidecar
has been published and synchronized. Cleanup removes the exact recorded source
files, prepared-file links, temporary guards, owned runtime staging, and the
private run directory. Working telemetry database copies and working logs are
removed along with the videos. The small final receipt remains in the game
directory as the run result and recovery record.

Cleanup checks file identities, including run metadata and externally configured
archive logs. Replaced files or unknown entries prevent
successful cleanup rather than being swept away. It never recursively purges the
shared game working directory. Shared model caches, game calibration, source
recordings, other runs, and persistent publication locks are not scratch.
Process diagnostics retain their separate bounded history as described in
[crash diagnostics](docs/crash-diagnostics.md).

Failures retain recoverable data. An interrupted publication may already have
some final filenames; retries verify those files and continue the same
transaction rather than allocating a second generation. A receipt marked
`published` still has cleanup to finish; only `complete` means the job succeeded.
Retries currently require the same filesystem mount identities; moving a receipt
to another machine or remounting its filesystems can fail identity verification.

Calibration preparation reservations are scratch rather than required outputs.
When calibration restarts the processing graph, the old telemetry database is
explicitly retired and only the final recording is required for publication.
Initial caps negotiation may also replace an empty telemetry database; that
replacement is refused if the previous database contains recorded samples.

## Desktop and legacy/direct runs

The UI starts managed publication only after normal CLI exit with code zero,
without a user Stop or incomplete calibration. Clicking **Stop** retains the
working archive and does not publish a completed recording. The UI uses the same
finalizer library in a background worker and prevents closing during publication.

Older runners that leave the manifest untouched retain the existing UI
publication path. A failed or malformed managed manifest cannot use this
fallback. The compatibility path finalizes videos individually and retains
working telemetry/logs and the reserved managed workspace; it does not provide
the managed job's cleanup contract.

A direct `hstream-cli` invocation without a managed run still writes under
`<output-root>/<game-id>/` and leaves its files there. `run.sh` does not add a
publication step. Direct `--encode-job` without a manifest writes MP4 in working
storage and currently does not enable faststart. Use an exported managed job for
an autonomous encode, publish, and cleanup lifecycle.

Source: [publication library](src/libs/publication/Publication.cpp),
[headless finalizer](src/apps/hstream-finalize/main.cpp),
[job exporter](src/apps/hstream-job/main.cpp),
[CLI output configuration](src/apps/hstream-cli/configurator.cpp), and
[desktop integration](src/apps/hstream-ui/HStreamWindow.cpp).
