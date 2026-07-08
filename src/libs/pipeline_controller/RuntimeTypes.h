#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hm::pipeline {

enum class RuntimeControlKind {
  Toggle,
  Integer,
  Float,
  Enum,
  Text,
};

enum class RuntimeControlApplyMode {
  Live,
  Paused,
  Ready,
  Restart,
};

using RuntimeValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

struct RuntimeChoiceValue {
  std::string id;
  std::string label;
  RuntimeValue value;
};

struct RuntimeControlDescriptor {
  std::string id;
  std::string group_id;
  std::string label;
  RuntimeControlKind kind{RuntimeControlKind::Text};
  RuntimeValue min;
  RuntimeValue max;
  RuntimeValue step;
  RuntimeValue value;
  RuntimeValue default_value;
  std::vector<RuntimeChoiceValue> choices;
  std::string unit;
  double display_scale{1.0};
  int precision{0};
  bool dirty{false};
  bool advanced{false};
  RuntimeControlApplyMode apply_mode{RuntimeControlApplyMode::Restart};
  bool live_writable{false};
  bool persisted{true};
  bool secret{false};
  bool unsafe{false};
  std::string source_id;
  std::string validation_error;
};

struct RuntimeControlGroup {
  std::string id;
  std::string label;
  std::vector<RuntimeControlDescriptor> controls;
};

enum class RuntimeOutputKind {
  Preview,
  RtmpPush,
  RtspServer,
  RtspClient,
  WebRtc,
  FileRecord,
};

enum class RuntimeOutputState {
  Disabled,
  Starting,
  Live,
  Reconnecting,
  Draining,
  Stopped,
  Error,
};

struct RuntimeOutputSpec {
  std::string id;
  RuntimeOutputKind kind{RuntimeOutputKind::Preview};
  int source_id{0};
  std::string tee_point;
  std::string uri;
  std::string host;
  int port{0};
  std::string mount_path;
  std::string output_file;
  int width{0};
  int height{0};
  int bitrate{0};
  bool include_audio{true};
  bool uri_contains_secret{false};
};

struct RuntimeOutputStatus {
  std::string id;
  RuntimeOutputKind kind{RuntimeOutputKind::Preview};
  RuntimeOutputState state{RuntimeOutputState::Stopped};
  std::string tee_point;
  std::string redacted_uri;
  std::string host;
  int port{0};
  std::string mount_path;
  std::string output_file;
  bool uri_contains_secret{false};
  uint64_t created_unix_ms{0};
  uint64_t frames{0};
  uint64_t bytes{0};
  int active_sessions{0};
  bool finalized{false};
  std::string last_error;
};

struct RuntimeStatus {
  bool configured{false};
  bool running{false};
  std::string state;
  std::string last_error;
};

struct PipelineLaunchConfig {
  std::vector<std::string> config_files;
  std::string game_id;
  std::vector<std::string> input_uris;
  std::vector<std::string> enabled_sources;
  std::vector<std::string> enabled_sinks;
  std::vector<std::pair<std::string, std::string>> pipeline_options;
  bool one_pass_stitching{true};
};

} // namespace hm::pipeline
