#pragma once

#include <QtGui/QIcon>

#include <array>

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
  Up,
  Down,
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
  NewFolder,
  Detection,
  Players,
  Count,
};

QIcon action_icon(ActionIcon action);
// Color platform icons and navigation arrows, and show standard dialog icons.
// Disabled icons retain their hue at reduced opacity.
void install_button_icon_style();

struct ActionIconSpec {
  const char* name;
  ActionIcon icon;
};

inline constexpr auto kActionIconSpecs = std::array{
    ActionIconSpec{"add", ActionIcon::Add},
    ActionIconSpec{"remove", ActionIcon::Remove},
    ActionIconSpec{"delete", ActionIcon::Delete},
    ActionIconSpec{"open", ActionIcon::Open},
    ActionIconSpec{"save", ActionIcon::Save},
    ActionIconSpec{"apply", ActionIcon::Apply},
    ActionIconSpec{"cancel", ActionIcon::Cancel},
    ActionIconSpec{"close", ActionIcon::Close},
    ActionIconSpec{"reset", ActionIcon::Reset},
    ActionIconSpec{"refresh", ActionIcon::Refresh},
    ActionIconSpec{"play", ActionIcon::Play},
    ActionIconSpec{"pause", ActionIcon::Pause},
    ActionIconSpec{"stop", ActionIcon::Stop},
    ActionIconSpec{"previous", ActionIcon::Previous},
    ActionIconSpec{"next", ActionIcon::Next},
    ActionIconSpec{"up", ActionIcon::Up},
    ActionIconSpec{"down", ActionIcon::Down},
    ActionIconSpec{"expand", ActionIcon::Expand},
    ActionIconSpec{"restore", ActionIcon::Restore},
    ActionIconSpec{"camera", ActionIcon::Camera},
    ActionIconSpec{"stitching", ActionIcon::Stitching},
    ActionIconSpec{"crop", ActionIcon::Crop},
    ActionIconSpec{"level", ActionIcon::Level},
    ActionIconSpec{"prepare", ActionIcon::Prepare},
    ActionIconSpec{"inspect", ActionIcon::Inspect},
    ActionIconSpec{"document", ActionIcon::Document},
    ActionIconSpec{"network", ActionIcon::Network},
    ActionIconSpec{"zoomin", ActionIcon::ZoomIn},
    ActionIconSpec{"zoomout", ActionIcon::ZoomOut},
    ActionIconSpec{"fit", ActionIcon::Fit},
    ActionIconSpec{"actualsize", ActionIcon::ActualSize},
    ActionIconSpec{"undo", ActionIcon::Undo},
    ActionIconSpec{"newfolder", ActionIcon::NewFolder},
    ActionIconSpec{"detection", ActionIcon::Detection},
    ActionIconSpec{"players", ActionIcon::Players},
};
static_assert(kActionIconSpecs.size() == static_cast<size_t>(ActionIcon::Count));

struct TabIconSpec {
  const char* label;
  ActionIcon icon;
};

// Shared with HockeyMONWeb by sync_stream_contract.py. Tab text may change
// after playback reports dimensions; its semantic icon stays attached.
inline constexpr auto kTabIconSpecs = std::array{
    TabIconSpec{"Program", ActionIcon::Play},        TabIconSpec{"Stitched", ActionIcon::Stitching},
    TabIconSpec{"Camera 1", ActionIcon::Camera},     TabIconSpec{"Camera 2", ActionIcon::Camera},
    TabIconSpec{"Camera 3", ActionIcon::Camera},     TabIconSpec{"Pipeline", ActionIcon::Network},
    TabIconSpec{"Tracking", ActionIcon::Inspect},    TabIconSpec{"Motion", ActionIcon::Next},
    TabIconSpec{"Rink framing", ActionIcon::Crop},   TabIconSpec{"Color", ActionIcon::Inspect},
    TabIconSpec{"Crop Rotation", ActionIcon::Crop},  TabIconSpec{"Runtime", ActionIcon::Prepare},
    TabIconSpec{"Detection", ActionIcon::Detection}, TabIconSpec{"Players", ActionIcon::Players},
    TabIconSpec{"Rotation", ActionIcon::Refresh},    TabIconSpec{"Color & Precision", ActionIcon::Inspect},
    TabIconSpec{"Alignment", ActionIcon::Stitching}, TabIconSpec{"Projection", ActionIcon::Crop},
    TabIconSpec{"Rink", ActionIcon::Level},          TabIconSpec{"Left camera", ActionIcon::Camera},
    TabIconSpec{"Right camera", ActionIcon::Camera}, TabIconSpec{"Preview", ActionIcon::Play},
    TabIconSpec{"Legacy", ActionIcon::Document},     TabIconSpec{"Optics", ActionIcon::Camera},
    TabIconSpec{"Panorama", ActionIcon::Stitching},  TabIconSpec{"Input", ActionIcon::Previous},
};

QIcon tab_icon(const QString& label);
