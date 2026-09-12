# Applications

Executable applications built from this repository.

Notable apps
- `hstream-cli`: Run DeepStream/GStreamer pipelines from the CLI. The existing `pipeline-app` target remains available
  as a compatibility/developer target while scripts migrate.
- `hstream-ui`: Qt desktop control-surface shell for in-process pipeline preview, outputs, and camera controls. The
  current target uses a disconnected demo backend while the reusable pipeline controller backend is being connected.
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
sbatch "$HOME/Videos/my-game/hstream-job.sh"
```

The generated Bash script directly executes `hstream-cli`; it never invokes `srun`
or `sbatch`. Its `#SBATCH` header requests one node, one task and one GPU. Add
site-specific account, partition, CPU, memory and wall-time directives with repeated
`--sbatch` options, or the UI's Slurm options field. No cluster allocation values
are copied from the exporter's current Slurm session. Use `--output PATH` for another
filename and `--force` to replace an existing script from the CLI.

The game config's `hstream_ui.job.arguments` records the UI's selected mode,
outputs and CLI overrides whenever settings are saved. Configs without this recipe
use the pipeline's configured sink enable flags. The script references the saved
game config, baseline, videos and assets; it does not copy them, so subsequent config
edits apply on execution. Paths must exist on the execution host. Use `--runner`,
`--config` and `--working-directory` to override automatically discovered Bazel or
installed paths. Disable **Render video** before exporting a headless batch job;
rendering in a script opens a standalone display instead of reusing UI window IDs.
UI-only preview overlays and interactive UI calibration dialogs are not attached
to standalone runs. Windows scripts run in the installed HStream WSL distribution.
