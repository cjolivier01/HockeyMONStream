#include "hstream/src/libs/stitching/RinkMaskFrameTime.h"
#include "hstream/src/libs/stitching/GameConfig.h"

#include <yaml-cpp/yaml.h>
#include <iostream>
#include <limits>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
} // namespace

int main() {
  using namespace hm::stitching;
  bool ok = true;
  for (const auto& [input, value] : std::vector<std::pair<std::string, int64_t>>{
           {"00:00:00", 0},
           {"00:10:00", 600000000000LL},
           {"-00:00:01", -1000000000LL},
           {"-00:00:00.125", -125000000},
           {"27:01:02.3", 97262300000000LL},
           // Shorthand: bare seconds, MM:SS, and single-digit fields.
           {"-1", -1000000000LL},
           {"0", 0},
           {"90", 90000000000LL},
           {"-1.5", -1500000000LL},
           {"1:30", 90000000000LL},
           {"-0:30", -30000000000LL},
           {"1:02:03", 3723000000000LL}}) {
    const auto parsed = ParseRinkMaskFrameTime(input);
    ok &= expect(parsed.ok() && parsed->has_value() && **parsed == value, "Signed time must parse correctly");
    if (parsed.ok()) {
      const auto again = ParseRinkMaskFrameTime(FormatRinkMaskFrameTime(*parsed));
      ok &= expect(again.ok() && *again == *parsed, "Canonical time must round trip");
    }
  }
  for (const char* bad :
       {"",
        "-",
        "-00:00:00",
        // Negative zero in every spelling the shorthand allows.
        "-0",
        "-0.000",
        "-0:00",
        "00:60:00",
        "00:00:60",
        "0:60",
        "1:60:00",
        "00:00:01.1234",
        "00:00:01x",
        "-auto",
        "1:2:3:4",
        "9999999:59:59.999"})
    ok &= expect(!ParseRinkMaskFrameTime(bad).ok(), "Invalid or overflowing times must fail");
  ok &= expect(
      ParseRinkMaskFrameTime("auto").ok() && !*ParseRinkMaskFrameTime("auto"), "auto must be distinct from zero");
  const auto end = ResolveRinkMaskFrameTime(-1000000000LL, 60000000000ULL);
  ok &= expect(end.ok() && *end == 59000000000ULL, "Negative time must use the full synchronized duration");
  ok &= expect(*ResolveRinkMaskFrameTime(0, 60000000000ULL) == 0, "Explicit zero must select recording start");
  ok &= expect(
      !ResolveRinkMaskFrameTime(60000000000LL, 60000000000ULL).ok() &&
          !ResolveRinkMaskFrameTime(-61000000000LL, 60000000000ULL).ok() && !ResolveRinkMaskFrameTime(-1, 0).ok() &&
          !ResolveRinkMaskFrameTime(-1, std::numeric_limits<uint64_t>::max()).ok(),
      "Out-of-range and unknown recording durations must fail");
  // The bundled baseline answers what the shipped behaviour is: the last
  // second of the recording, for every venue and for games with none selected.
  const YAML::Node shipped;
  ok &= expect(
      read_rink_mask_frame_time_default(shipped).ok() && *read_rink_mask_frame_time_default(shipped) == "-00:00:01",
      "The bundled baseline must default the ice mask frame to one second before the end");
  ok &= expect(
      read_rink_mask_frame_time(shipped).ok() && *read_rink_mask_frame_time(shipped) == "-00:00:01",
      "A game with no rink and no selection must inherit that default");
  for (const char* venue : {"vallco", "sharks-ice", "tv-dublin", "fremont"}) {
    YAML::Node venue_config;
    venue_config["stitching"]["rink_config"] = venue;
    ok &= expect(
        read_rink_mask_frame_time(venue_config).ok() && *read_rink_mask_frame_time(venue_config) == "-00:00:01",
        "Every bundled venue must inherit the same ice mask frame");
  }

  YAML::Node config = YAML::Load(R"(
stitching:
  rink_configs:
    late:
      display_name: Late rink
      rotation_degrees: [0, 0, 0]
      rink_mask_frame_time: '-00:00:01'
    ordinary:
      display_name: Ordinary rink
      rotation_degrees: [0, 0, 0]
  rink_config: late
  rink_mask_frame_time_default: '-00:00:03'
)");
  ok &= expect(*read_rink_mask_frame_time(config) == "-00:00:01", "Selected rink must supply its default");
  config["stitching"]["rink_mask_frame_time"] = "00:00:00";
  ok &= expect(*read_rink_mask_frame_time(config) == "00:00:00", "Explicit zero must override a rink default");
  config["stitching"]["rink_mask_frame_time"] = "auto";
  ok &= expect(*read_rink_mask_frame_time(config) == "auto", "Automatic must override a rink default");
  config["stitching"]["rink_mask_frame_time"] = YAML::Node(YAML::NodeType::Null);
  ok &= expect(*read_rink_mask_frame_time(config) == "-00:00:01", "Null must restore rink inheritance");
  config["stitching"]["rink_config"] = "ordinary";
  ok &= expect(
      *read_rink_mask_frame_time(config) == "-00:00:03",
      "A venue without its own time must inherit the configured default");
  config["stitching"]["rink_mask_frame_time_default"] = "auto";
  ok &=
      expect(*read_rink_mask_frame_time(config) == "auto", "An automatic configured default must reach that venue too");
  config["stitching"]["rink_mask_frame_time_default"] = "-00:00:00";
  ok &= expect(
      !read_rink_mask_frame_time_default(config).ok() && !read_rink_mask_frame_time(config).ok(),
      "An invalid configured default must fail instead of selecting another frame");
  config["stitching"]["rink_mask_frame_time_default"] = "-3";
  ok &= expect(*read_rink_mask_frame_time(config) == "-00:00:03", "Configured defaults must accept the same shorthand");
  YAML::Node effective = YAML::Clone(config);
  effective["stitching"]["rink_mask_frame_time"] = "00:00:12.5";
  ok &= expect(
      materialize_rink_mask_frame_time(config, effective).ok() && *read_rink_mask_frame_time(config) == "00:00:12.500",
      "Workers must receive the effective layered selection");
  ok &= expect(
      restore_generated_rink_mask_frame_time(config) && config["stitching"]["rink_mask_frame_time"].IsNull(),
      "Restoring generated time must retain explicit inheritance");
  effective["stitching"]["rink_config"] = "late";
  effective["stitching"]["rink_configs"]["late"]["rink_mask_frame_time"] = "-00:00:02";
  ok &= expect(
      materialize_stitch_rink_context(config, effective).ok() && *read_rink_mask_frame_time(config) == "-00:00:02",
      "Materialized profiles must retain the mask-frame default");
  restore_generated_stitch_rink_context(config);
  // Start from a document that knows no venues, so materialization has to
  // write the catalog instead of accepting a matching one.
  config["stitching"].remove("rink_configs");
  config["stitching"]["rink_config"] = YAML::Node(YAML::NodeType::Null);
  effective["stitching"]["rink_config"] = "ordinary";
  const auto materialized = materialize_stitch_rink_context(config, effective);
  const YAML::Node written = config["stitching"]["rink_configs"];
  ok &= expect(
      materialized.ok() && *materialized && written["ordinary"]["display_name"] &&
          !written["ordinary"]["rink_mask_frame_time"] &&
          written["late"]["rink_mask_frame_time"].as<std::string>() == "-00:00:02" &&
          *read_rink_mask_frame_time(config) == "-00:00:03",
      "A venue with no mask-frame override must stay on the configured default in worker documents");
  return ok ? 0 : 1;
}
