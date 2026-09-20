# Pipeline view navigation

The desktop UI's Pipeline tab inspects the running GStreamer graph. Click
Refresh to update topology and live state. Refresh preserves your zoom,
position, and selected node if it still exists in the same pipeline session;
a new stage or generation clears the old selection and fits the new graph.

Click a node or bin background to select it. A bright blue outline and lighter
fill identify the selection, and the properties pane immediately shows its
name and path while live values load. Connection lines do not block clicks on
the bin behind them.

| Control | Action |
| --- | --- |
| Mouse wheel / vertical trackpad scroll | Zoom around the pointer, including from large graph overviews |
| + / − buttons or keys | Zoom around the view center |
| 100% button / `1` | Reset to actual size |
| Fit button / `F` / `Home` | Show the whole graph |
| Focus selected button / `S` | Frame the selected node or bin |
| Double-click node or bin | Select and frame it |
| Double-click empty space | Show the whole graph |
| Drag empty space or bin background | Pan; a bin click without a drag selects it |
| Middle-button drag anywhere | Pan while keeping the selection |
| Arrow keys | Scroll the graph |
| `Esc` | Clear selection |
| `Ctrl+F` | Focus node search |
| Enter in node search / Find next | Select and frame the next matching name, factory, or path |

Graph keys apply when the graph has keyboard focus. Text and property editors
keep their normal key handling. The zoom percentage appears beside the zoom
buttons. The corner maximize button expands the graph while keeping properties
visible. Hover over the graph for navigation hints.

`src/apps/hstream-ui/PipelineInspectorWidget.cpp` owns rendering and navigation.
Topology and property requests still use the CLI inspector protocol and its
stage/generation checks; navigation does not mutate the running pipeline.

Run the UI-only interaction regression (including a 151-node graph, pointer
anchoring, selection pixels, refresh, and keyboard/mouse navigation) with:

```bash
bazelisk test --config=opt --cpu=k8 //src/apps/hstream-ui:pipeline_inspector_widget_test --test_output=errors
```

The target uses Qt's offscreen platform and does not require video or a GPU.
Qt UI targets are currently excluded from `--config=jetson` builds.
