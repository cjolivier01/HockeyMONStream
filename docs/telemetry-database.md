# Telemetry databases

Enable **DriveGPT database** before a Program run. Hstream writes one SQLite file in `~/hstream_output/<game-id>` (or `HM_OUTPUT_WORK_DIR`). On successful pipeline shutdown, the UI copies a closed snapshot to the game directory as `hstream_telemetry-N.db`, with the finalized video's suffix. Without video encoding, it chooses the next available generation. Existing recordings are never overwritten. The working file may initially be unsuffixed; subsequent working generations increase beyond existing database/manifest suffixes.

Direct runs enable recording with:

```sh
--options=pipeline.ds-playtracker.private-properties.telemetry-db-dir=/path/to/output
--options=pipeline.ds-playtracker.private-properties.telemetry-game-id=my-game
```

`telemetry-csv-dir` remains a configuration alias and now writes a database. Source game identity defaults to the output directory basename if not explicitly supplied. The UI supplies it explicitly. Keep game IDs consistent across repeated passes over the same physical game.

## Contents and identities

`src/libs/recording/schema.sql` is identical to `../hm/hmlib/telemetry/schema.sql`. The application ID and schema version identify the format; readers reject unsupported versions.

- `runs`: UUID, source game ID, producer/start time, original/effective configurations, completion/outcome and sample count.
- `frames`: unique sample sequence, original/source timestamps and frame IDs, seek/reset identities, dimensions through a geometry reference, and detection/track counts.
- `detections`, `tracks`, `cameras`: original stitched-coordinate observations and fast/Program camera outputs. Detection/track ordinals preserve original input ordering. Track IDs use decimal strings to retain unsigned 64-bit values.
- `replay_frames`, `replay_tracks`, `checkpoints`: exact native TLBR inputs, wrapper state, rotations, and periodic complete native snapshots/base configurations captured before the associated step.
- `config_events`: effective sample boundaries and configuration contents.
- The startup `run-configuration` event contains `run-config.yaml`: the full resolved launch YAML (including CLI overrides and generated settings), loaded baseline/user/game layers, app/subconfiguration documents, current game settings, and referenced text config contents. Layer snapshots preserve parsed values; referenced files preserve raw text. Missing optional files and directory references are explicit. Binary models/video remain external. This archive is captured once before startup and survives database publication/merging under the run UUID; existing tracker runtime changes continue to be recorded at their effective sample boundaries.
- `geometries`: lossless PNG rink mask, native canvas dimensions, coordinate-space name, identity transform for native masks, geometry revision and SHA-256. `rink_inputs` reserves a versioned representation for model-specific features; hstream stores the original mask so training can derive the input at its configured resolution and normalization.

All rows belong to a run UUID. Copying or merging preserves that UUID; a new processing pass creates a new UUID. Child keys combine it with sample sequence and an ordinal/role. Seeks and live policy changes retain explicit boundaries. Geometry changes create a new revision referenced by frames.

## Performance and durability

Only CPU metadata enters the bounded lossless queue (2048 items by default). A single writer uses prepared statements and transactions batched at 120 work items. Queue saturation blocks producers instead of dropping samples. Writer errors fail the recording and surface to the pipeline. The database remains incomplete until the pipeline has stopped successfully and explicitly finalizes telemetry; downstream shutdown failures invalidate it.

Rink capture shares the immutable CPU calibration mask already held by field masking. PNG compression and hashing happen once per geometry on the writer thread. It does not map/read video surfaces or introduce GPU-to-CPU copies. At 16,000 × 6,500 the mask's existing CPU allocation is retained until encoding completes; there is no panorama encoding or per-frame mask serialization.

Native checkpoints remain periodic (normally every 120 frames, plus initialization and policy/reset boundaries). Full state serialization on every frame is not introduced. SQLite uses FULL synchronization and a rollback journal; a journal can exist while recording. A completed file is closed and self-contained. Game-directory publication uses a SQLite snapshot, verifies its integrity, synchronizes it, and links it without replacement before synchronizing the directory.

## Consumers

Camera experiments read the database directly, select a run, query the checkpoint before the requested in point, and replay exact native inputs to reconstruct full state. Trials clone that state. Preview hardware-decodes original camera chapters and stitches on the GPU, or uses an uncropped proportionally scaled archive. Preserve the historical source configuration and stitching maps; media/time binding is still explicit. A 16K-wide canvas is never required to be encoded for this workflow.

HockeyMON PR https://github.com/cjolivier01/HockeyMON/pull/167 adds direct DriveGPT database training, portable dataset publication, and transactional merging. `--database` accepts repeated paths/directories/globs and any mixture of individual and merged files. Repeated identical run UUIDs are deduplicated; conflicting content fails. Training groups all runs of the same source game into one train/validation side and prevents sequences from crossing geometry/policy/reset boundaries. Legacy CSV recordings remain readable.

See [camera experiments](camera-experiments.md) and HockeyMON's `docs/telemetry-databases.md` for commands.
