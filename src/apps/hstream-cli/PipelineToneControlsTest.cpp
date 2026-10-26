// As in PipelineApp.cpp, consume GLX before PipelineApp.h undefines X11 Status.
#include "hstream/src/libs/common/Status.h"
#include "src/apps/hstream-cli/PipelineApp.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

std::mutex output_mutex;
std::string output;
void Capture(const gchar* text) {
  std::lock_guard<std::mutex> lock(output_mutex);
  output += text;
}
std::string Output() {
  std::lock_guard<std::mutex> lock(output_mutex);
  return output;
}
void ClearOutput() {
  std::lock_guard<std::mutex> lock(output_mutex);
  output.clear();
}

struct ToneElement {
  GstElement parent;
  gdouble shadows, exposure;
  gboolean black, high_bit_depth, accepted, reject_exposure;
  guint mutations;
};
struct ToneElementClass {
  GstElementClass parent;
};
enum Property { kNone, kShadows, kBlack, kExposure, kHighBit, kAccepted };
std::function<void()> before_exposure;
G_DEFINE_TYPE(ToneElement, tone_element, GST_TYPE_ELEMENT)

void Set(GObject* object, guint property, const GValue* value, GParamSpec*) {
  auto* element = reinterpret_cast<ToneElement*>(object);
  ++element->mutations;
  element->accepted = TRUE;
  if (property == kShadows)
    element->shadows = g_value_get_double(value);
  else if (property == kBlack)
    element->black = g_value_get_boolean(value);
  else if (property == kHighBit)
    element->high_bit_depth = g_value_get_boolean(value);
  else if (property == kExposure) {
    if (before_exposure)
      before_exposure();
    element->accepted = !element->reject_exposure;
    if (element->accepted)
      element->exposure = g_value_get_double(value);
  }
}
void Get(GObject* object, guint property, GValue* value, GParamSpec*) {
  auto* element = reinterpret_cast<ToneElement*>(object);
  if (property == kShadows)
    g_value_set_double(value, element->shadows);
  else if (property == kBlack)
    g_value_set_boolean(value, element->black);
  else if (property == kExposure)
    g_value_set_double(value, element->exposure);
  else if (property == kHighBit)
    g_value_set_boolean(value, element->high_bit_depth);
  else if (property == kAccepted)
    g_value_set_boolean(value, element->accepted);
}
void tone_element_class_init(ToneElementClass* type) {
  auto* object = G_OBJECT_CLASS(type);
  object->set_property = Set;
  object->get_property = Get;
  const auto flags = GParamFlags(G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING);
  g_object_class_install_property(
      object, kShadows, g_param_spec_double("shadow-lift", "Shadows", "Shadows", 0, 100, 0, flags));
  g_object_class_install_property(
      object, kBlack, g_param_spec_boolean("shadow-lift-black-point", "Black", "Black", FALSE, flags));
  g_object_class_install_property(
      object, kExposure, g_param_spec_double("exposure", "Exposure", "Exposure", 0, 1.3, 0, flags));
  g_object_class_install_property(
      object, kHighBit, g_param_spec_boolean("high-bit-depth", "Precision", "Precision", FALSE, flags));
  g_object_class_install_property(
      object, kAccepted, g_param_spec_boolean("last-property-set-ok", "Accepted", "Accepted", TRUE, G_PARAM_READABLE));
}
void tone_element_init(ToneElement* element) {
  element->accepted = TRUE;
}
} // namespace

struct PipelineToneControlsTestAccess {
  PipelineApplication application;
  std::shared_ptr<HmApp> context = std::make_shared<HmApp>("fixture", "", 0);
  GstElement* pipeline{nullptr};
  ToneElement* cropper{nullptr};
  ToneElement* stitcher{nullptr};
  uint64_t next_request{0};

  PipelineToneControlsTestAccess() {
    application.stage_app_contexts_[0] = {context};
    application.inspector_topology_generation_ = 1;
    ReplaceGraph();
  }
  ~PipelineToneControlsTestAccess() {
    before_exposure = {};
    context->pipeline.pipeline = nullptr;
    gst_object_unref(pipeline);
  }
  void ReplaceGraph() {
    GstElement* replacement = gst_pipeline_new(nullptr);
    cropper = reinterpret_cast<ToneElement*>(g_object_new(tone_element_get_type(), "name", "playcropper0", nullptr));
    stitcher = reinterpret_cast<ToneElement*>(g_object_new(tone_element_get_type(), "name", "hmstitcher0", nullptr));
    gst_bin_add_many(GST_BIN(replacement), GST_ELEMENT(cropper), GST_ELEMENT(stitcher), nullptr);
    context->pipeline.pipeline = replacement;
    if (pipeline)
      gst_object_unref(pipeline);
    pipeline = replacement;
  }
  std::string Command(const std::string& controls, long stage = 0, uint64_t generation = 1) {
    return "set-tone-controls " + std::to_string(++next_request) + " " + std::to_string(stage) + " " +
        std::to_string(generation) + " " + controls;
  }
  void Rejected(const std::string& command, const char* reason) {
    const auto mutations = cropper->mutations + stitcher->mutations;
    const auto overrides = application.runtime_property_overrides_.size();
    ClearOutput();
    Check(!application.handle_runtime_command_line(command), "Invalid tone command was applied");
    Check(
        Output().find("HSTREAM_TONE_CONTROLS ") != std::string::npos, "Tone rejection lacks structured acknowledgment");
    Check(Output().find(std::string("\"reason\":\"") + reason + "\"") != std::string::npos, "Wrong rejection reason");
    Check(cropper->mutations + stitcher->mutations == mutations, "Rejected command touched properties");
    Check(application.runtime_property_overrides_.size() == overrides, "Rejected command changed replay state");
  }
  std::string Query(long stage = 0, uint64_t generation = 1) {
    return "query-tone-controls " + std::to_string(++next_request) + " " + std::to_string(stage) + " " +
        std::to_string(generation);
  }
  void Queries() {
    auto descriptor = [&] {
      const auto mutations = cropper->mutations + stitcher->mutations;
      ClearOutput();
      Check(application.handle_runtime_command_line(Query()), "Current tone query failed");
      Check(Output().find("\"kind\":\"descriptor\"") != std::string::npos, "Tone query did not return descriptor");
      Check(cropper->mutations + stitcher->mutations == mutations, "Tone query mutated the graph");
      Check(application.runtime_property_overrides_.empty(), "Tone query created replay overrides");
    };
    cropper->shadows = 35;
    cropper->black = TRUE;
    cropper->exposure = static_cast<float>(0.61);
    descriptor();
    Check(Output().find("\"highBitDepth\":false") != std::string::npos, "Descriptor lost actual precision");
    Check(
        Output().find("\"id\":\"Exposure_x100\",\"value\":61") != std::string::npos, "Descriptor lost native readback");
    Check(
        Output().find("\"maximum\":130,\"step\":1,\"divisor\":100") != std::string::npos,
        "Descriptor lost shared units");
    Rejected("query-tone-controls " + std::to_string(next_request) + " 0 1", "stale-request");
    Rejected(Query() + " Exposure_x100=5", "invalid-command");
    Rejected(Query(-1), "stale-session");
    application.runtime_seek_pending_.emplace();
    Rejected(Query(), "seeking");
    application.runtime_seek_pending_.reset();
    for (double value :
         {0.615, -0.1, 1.4, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
      cropper->exposure = value;
      Rejected(Query(), "unrepresentable-value");
      Check(Output().find("\"controls\"") == std::string::npos, "Failed readback leaked partial authoritative values");
    }
    cropper->exposure = 0.61;
    application.stitching_calibration_only_ = true;
    Rejected(Query(), "ineligible-calibration");
    stitcher->high_bit_depth = TRUE;
    stitcher->exposure = 1.3;
    descriptor();
    Check(
        Output().find("\"element\":\"hmstitcher0\"") != std::string::npos, "Descriptor selected wrong high-bit owner");
    Check(
        Output().find("\"id\":\"Exposure_x100\",\"value\":130") != std::string::npos,
        "Descriptor read wrong high-bit owner");
    application.stitching_calibration_only_ = false;
    stitcher->high_bit_depth = FALSE;
    gst_object_ref(cropper);
    gst_bin_remove(GST_BIN(pipeline), GST_ELEMENT(cropper));
    Rejected(Query(), "tone-unavailable");
    gst_bin_add(GST_BIN(pipeline), GST_ELEMENT(cropper));
    descriptor();
  }
  void Run() {
    const auto first = Command("Exposure_x100=61 Bring_Up_Shadows=35 Lift_Shadow_Black_Point=1");
    ClearOutput();
    Check(application.handle_runtime_command_line(first), "Valid typed tone batch failed");
    Check(
        cropper->shadows == 35 && cropper->exposure == 0.61 && cropper->black && stitcher->mutations == 0,
        "Tone command changed the wrong owner/units");
    Check(
        Output().find("\"requestId\":1,\"status\":\"ok\",\"stage\":0,\"generation\":1") != std::string::npos,
        "Successful tone acknowledgment lost its request/session identity");
    Check(
        Output().find("runtime property ") == std::string::npos,
        "Typed command emitted an ambiguous legacy acknowledgment");
    Rejected(first, "stale-request");
    for (const char* bad :
         {"",
          "Exposure_x100=131",
          "Exposure_x100=0.5",
          "Bring_Up_Shadows=101",
          "Lift_Shadow_Black_Point=2",
          "Exposure_x100=-1",
          "Exposure_x100=nan",
          "Exposure_x100=999999999999999999999999",
          "runtime-tuning-config-file=/tmp/file",
          "Exposure_x100=1 Exposure_x100=2",
          "Exposure_x100=1;playcropper0",
          "unknown=1"})
      Rejected(Command(bad), "invalid-command");
    Rejected("set-tone-controls 0 0 1 Exposure_x100=1", "invalid-command");
    Rejected("set-tone-controls " + std::string(1100, '1'), "invalid-command");
    const auto invalid_once = Command("Exposure_x100=131");
    Rejected(invalid_once, "invalid-command");
    Rejected(invalid_once, "stale-request");
    Rejected(Command("Exposure_x100=1", -1), "stale-session");
    Rejected(Command("Exposure_x100=1", 0, 2), "stale-session");

    application.begin_pipeline_recreation();
    Rejected(Command("Exposure_x100=1"), "reconstructing");
    application.end_pipeline_recreation(false);
    application.runtime_seek_pending_.emplace();
    Rejected(Command("Exposure_x100=1"), "seeking");
    application.runtime_seek_pending_.reset();
    application.runtime_seek_recreation_active_ = true;
    Rejected(Command("Exposure_x100=1"), "reconstructing");
    application.runtime_seek_recreation_active_ = false;
    application.stage_app_contexts_[0].push_back(context);
    Rejected(Command("Exposure_x100=1"), "unsupported-topology");
    application.stage_app_contexts_[0].pop_back();

    // The third setter rejects after the first two succeeded. The real batch
    // transaction restores both and must not commit any of the failed values.
    cropper->reject_exposure = TRUE;
    const auto override_count = application.runtime_property_overrides_.size();
    ClearOutput();
    Check(
        !application.handle_runtime_command_line(
            Command("Bring_Up_Shadows=90 Lift_Shadow_Black_Point=0 Exposure_x100=99")),
        "Plugin rejection was acknowledged as success");
    Check(
        Output().find("property-rejected: playcropper0.exposure") != std::string::npos,
        "Plugin rejection lost its exact property");
    Check(cropper->shadows == 35 && cropper->black && cropper->exposure == 0.61, "Rejected batch did not roll back");
    Check(application.runtime_property_overrides_.size() == override_count, "Rejected batch polluted replay state");
    cropper->reject_exposure = FALSE;
    application.begin_pipeline_recreation();
    ReplaceGraph();
    ClearOutput();
    Check(application.reapply_runtime_properties(), "Accepted tone values failed to replay after reconstruction");
    Check(
        cropper->shadows == 35 && cropper->black && cropper->exposure == 0.61,
        "Replay restored rejected/incorrect values");
    Check(Output().find("HSTREAM_TONE_CONTROLS") == std::string::npos, "Replay forged a new request acknowledgment");
    application.end_pipeline_recreation(true);
    Rejected(Command("Exposure_x100=1"), "stale-session");

    // Parse while waiting for the graph lock, then change the topology before
    // admission. This must reject at apply time rather than using a snapshot.
    auto lock = std::unique_lock<std::mutex>(application.pipeline_access_mu_);
    const auto delayed = Command("Exposure_x100=1", 0, 2);
    ClearOutput();
    auto pending = std::async(std::launch::async, [&] { return application.handle_runtime_command_line(delayed); });
    const bool waiting = pending.wait_for(30ms) == std::future_status::timeout;
    application.inspector_topology_generation_ = 3;
    lock.unlock();
    Check(waiting && !pending.get(), "Request admitted against a topology that changed while it waited");
    Check(
        Output().find("\"reason\":\"stale-session\"") != std::string::npos && cropper->exposure == 0.61,
        "Stale queued request changed the replacement pipeline");

    application.stitching_calibration_only_ = true;
    Rejected(Command("Exposure_x100=1", 0, 3), "ineligible-calibration");
    stitcher->high_bit_depth = TRUE;
    Check(
        application.handle_runtime_command_line(Command("Exposure_x100=130", 0, 3)),
        "High-bit calibration tone rejected");
    Check(stitcher->exposure == 1.3 && cropper->exposure == 0.61, "High-bit calibration changed the cropper");
    application.stitching_calibration_only_ = false;
    application.current_stage_ = -1;
    application.stage_app_contexts_[-1] = {context};
    stitcher->high_bit_depth = FALSE;
    Rejected(Command("Exposure_x100=1", -1, 3), "ineligible-calibration");
    stitcher->high_bit_depth = TRUE;
    Check(
        application.handle_runtime_command_line(Command("Exposure_x100=130", -1, 3)),
        "Negative calibration stage rejected high-bit tone controls");
    application.current_stage_ = 0;
    application.stage_app_contexts_.erase(-1);

    // A write being accepted is not a completed mutation. Observe the real
    // setter in flight and verify the correlated success appears only after it.
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, release = false;
    before_exposure = [&] {
      std::unique_lock<std::mutex> guard(mutex);
      entered = true;
      condition.notify_all();
      condition.wait_for(guard, 3s, [&] { return release; });
    };
    ClearOutput();
    const auto blocked = Command("Exposure_x100=72", 0, 3);
    auto applying = std::async(std::launch::async, [&] { return application.handle_runtime_command_line(blocked); });
    bool did_enter = false, premature_ack = false;
    {
      std::unique_lock<std::mutex> guard(mutex);
      did_enter = condition.wait_for(guard, 3s, [&] { return entered; });
      premature_ack = Output().find("HSTREAM_TONE_CONTROLS") != std::string::npos;
      release = true;
    }
    condition.notify_all();
    const bool completed = applying.get();
    before_exposure = {};
    Check(
        did_enter && !premature_ack && completed && stitcher->exposure == 0.72,
        "Tone acknowledgment preceded actual completion");

    ClearOutput();
    Check(
        application.handle_runtime_command_line("set-properties playcropper0 exposure=0.29"), "Legacy Qt batch failed");
    Check(
        Output().find("runtime property playcropper0 exposure=0.29\n") != std::string::npos &&
            Output().find("HSTREAM_TONE_CONTROLS") == std::string::npos,
        "Legacy Qt/HockeyMON acknowledgment wire changed");
  }
};

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  const auto previous = g_set_print_handler(Capture);
  try {
    PipelineToneControlsTestAccess test;
    test.Run();
    PipelineToneControlsTestAccess queries;
    queries.Queries();
    g_set_print_handler(previous);
    return 0;
  } catch (const std::exception& error) {
    g_set_print_handler(previous);
    std::cerr << error.what() << '\n' << Output();
    return 1;
  }
}
