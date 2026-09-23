#pragma once

#include <QtGui/QIcon>

enum class ActionIcon {
  Add,
  Remove,
  Delete,
  Open,
  Save,
  Apply,
  Cancel,
  Close,
  Reset,
  Refresh,
  Play,
  Pause,
  Stop,
  Previous,
  Next,
  Expand,
  Restore,
  Camera,
  Stitching,
  Crop,
  Level,
  Prepare,
  Inspect,
  Document,
  Network,
  ZoomIn,
  ZoomOut,
  Fit,
  ActualSize,
  Undo,
};

QIcon action_icon(ActionIcon action);
// Color platform icons and navigation arrows, and show standard dialog icons.
// Disabled icons retain their hue at reduced opacity.
void install_button_icon_style();
