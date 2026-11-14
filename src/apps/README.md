# Applications

Executable applications built from this repository.

Notable apps
- `hstream-ui`: Main Qt desktop entry point for preview, outputs, calibration, and camera controls. It launches
  `hstream-cli` as a child process and sends runtime commands through stdin.
- `hstream-cli`: Main command-line entry point for DeepStream/GStreamer pipelines. Build with `make hstream-cli`
  or `bazelisk build --config=opt //src/apps/hstream-cli:hstream-cli`.
- `dual-record`: Dual-camera recorder utilities and daemon.

Docs
- Build: `bazelisk build //docs:site`
- Open: `../../bazel-bin/docs/site_html/html/index.html`

Save the current UI job with **File → Save job script…**. File, Run and Tools menus
also expose game/preset, playback and calibration actions; transport buttons remain
available. Export saves the current settings and invokes `hstream-job`, without
starting playback. The default destination is `<game-dir>/hstream-job.sh`.

The same exporter is available from the command line:

```bash
make hstream-job hstream-cli
bazel-bin/src/apps/hstream-job/hstream-job --game-dir "$HOME/Videos/my-game"
# Installed Debian package (also inside the Windows installer's HStream WSL shell):
hstream-job --game-dir "$HOME/Videos/my-game" \
  --sbatch '--partition=gpu' --sbatch '--time=02:00:00'

"$HOME/Videos/my-game/hstream-job.sh"
# Alternatively, submit the same file yourself:
(cd "$HOME/Videos/my-game" && sbatch ./hstream-job.sh)
```

The generated Bash script supervises `hstream-cli` through `hstream-finalize`; it never invokes `srun`
or `sbatch`. Its `#SBATCH` header requests one node, one task and one GPU. Add
site-specific account, partition, CPU, memory and wall-time directives with repeated
`--sbatch` options, or the UI's Slurm options field. No cluster allocation values
are copied from the exporter's current Slurm session. Use `--output PATH` for another
filename and `--force` to replace an existing script from the CLI.

The game config's `hstream_ui.job.arguments` records the UI's selected mode,
outputs and CLI overrides whenever settings are saved. With no selected file
encoder, exports use the baseline's 4K Program archive selection. The script references the saved
game config, baseline, videos and assets; it does not copy them, so subsequent config
edits apply on execution. Ordinary exports locate the game relative to the script;
they can move together. To copy only the script, set `HM_GAME_DIR` to the execution
host's games root. Slurm spool copies use `SLURM_SUBMIT_DIR` as the script directory,
so submit from that directory or set `HM_GAME_DIR`. Use `--runner`, `--finalizer`,
`--config` and `--working-directory` for explicit runtime overrides. Exported
encoding jobs disable rendering and retain the selected file encoders.
UI-only preview overlays and interactive UI calibration dialogs are not attached
to standalone runs. Windows scripts run in the installed HStream WSL distribution.

Each ordinary script invocation creates a fresh run, publishes completed MP4s and
DriveGPT databases into the game, and removes that run's scratch after success.
No previous run directory is required. Working storage comes from the execution
host's configuration or `HM_OUTPUT_WORK_DIR`; export-host environment paths are
not saved. The finalizer must support `--new-run`.

For explicit recovery, use `hstream-finalize --manifest RECEIPT.yaml`. Supervised
exports using `--run-id ID` or `--interactive-publication` keep their fixed identity
and resume semantics. See [LIFECYCLE.md](../../LIFECYCLE.md).
