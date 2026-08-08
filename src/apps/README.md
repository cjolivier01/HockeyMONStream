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
