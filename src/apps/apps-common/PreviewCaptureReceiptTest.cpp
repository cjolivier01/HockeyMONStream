#include "PreviewCaptureReceipt.h"

#include <json-glib/json-glib.h>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* reason) {
  if (!value)
    throw std::runtime_error(reason);
}
const char* Plan =
    R"({"version":1,"requests":[{"id":"same-physical-frame","channel":"program","leftUri":"file:///left.mp4","rightUri":"file:///right.mp4","leftPtsNs":"1001000000","rightPtsNs":"2002000000","count":3}]})";
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  using namespace hm::preview;
  try {
    CapturePlan plan(Plan);
    CaptureMetadata metadata;
    metadata.pair = hm::StitchingFramePair{
        {0, 60, g_quark_from_static_string("file:///left.mp4"), 1001000000},
        {1, 60, g_quark_from_static_string("file:///right.mp4"), 2002000000},
        0,
        3840,
        2160,
        3840,
        2160};
    hm::preview_overlay::PlayCropperTransform crop;
    crop.input_width = 3840;
    crop.input_height = 2160;
    crop.metadata_width = 3840;
    crop.metadata_height = 2160;
    crop.crop_left = 100;
    crop.crop_top = 50;
    crop.crop_width = 1920;
    crop.crop_height = 1080;
    crop.output_width = 1280;
    crop.output_height = 720;
    crop.anchor_x = 960;
    crop.anchor_y = 540;
    metadata.crop = crop;
    CaptureContext context;
    context.interface = "browser";
    context.run = "run";
    context.channel = "program";
    context.topology = 1;
    context.operation = 1;
    context.sequence = 10;
    context.source_pts = 0;
    Check(!plan.Capture(context, {}), "Missing source metadata cannot qualify");
    auto missing_crop = metadata;
    missing_crop.crop.reset();
    Check(!plan.Capture(context, missing_crop), "Program requires its exact frame crop");
    auto wrong = metadata;
    wrong.pair->left.source_pts++;
    Check(!plan.Capture(context, wrong), "Equal local PTS cannot hide different physical PTS");
    wrong = metadata;
    wrong.pair->right.source_uri = g_quark_from_static_string("file:///other.mp4");
    Check(!plan.Capture(context, wrong), "Wrong chapter must not match");
    const auto first = plan.Capture(context, metadata);
    Check(first.has_value(), "Exact physical pair should capture");
    Check(!plan.Capture(context, metadata), "Duplicate frame cannot consume another occurrence");
    auto* parser = json_parser_new();
    Check(json_parser_load_from_data(parser, first->c_str(), -1, nullptr), "Receipt must parse");
    auto* object = json_node_get_object(json_parser_get_root(parser));
    Check(json_object_get_boolean_member(object, "physicalIdentityValid"), "Physical identity valid");
    auto* actual_crop = json_object_get_object_member(object, "crop");
    Check(
        json_object_get_double_member(actual_crop, "left") == 100 &&
            json_object_get_double_member(actual_crop, "width") == 1920,
        "Serialize actual crop");
    auto* sources = json_object_get_array_member(object, "physicalSources");
    Check(json_array_get_length(sources) == 2, "Both physical cameras retained");
    Check(
        std::string(json_object_get_string_member(json_array_get_object_element(sources, 1), "ptsNs")) == "2002000000",
        "Original source PTS preserved");
    g_object_unref(parser);
    // Reconstructed local PTS and sequence may repeat, while the physical frame
    // remains the same. Topology disambiguates the bounded second observation.
    context.topology = 2;
    Check(plan.Capture(context, metadata).has_value(), "Reconstruction can capture same physical pair");
    context.topology = 1;
    Check(!plan.Capture(context, metadata), "Interleaved duplicate must not consume the final occurrence");
    context.topology = 3;
    Check(plan.Capture(context, metadata).has_value(), "Final unique frame consumes the budget");
    Check(!plan.pending(), "Exhausted plan stops metadata sampling");
    context.topology = 4;
    Check(!plan.Capture(context, metadata), "Budget must survive topology replacement");
    CapturePlan disabled(nullptr);
    Check(disabled.empty() && !disabled.Capture(context, metadata), "Omitted opt-in is inert");
    for (const auto* invalid : {"{}", "null", "{\"version\":1,\"requests\":[]}"}) {
      bool rejected = false;
      try {
        CapturePlan bad(invalid);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      Check(rejected, "Invalid explicit capture plan must fail before runtime");
    }
    std::string too_many = Plan;
    too_many.replace(too_many.find("\"count\":3"), 9, "\"count\":4");
    bool rejected = false;
    try {
      CapturePlan bad(too_many.c_str());
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "Capture occurrence budget enforced");
    std::cout << "Bounded physical-frame/crop capture receipts passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
