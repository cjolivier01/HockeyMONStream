#include "WebPreviewOptions.h"
#include "WebPreviewRuntime.h"
#include "hstream/src/libs/program/PreviewOverlayConfig.h"

#include <json-glib/json-glib.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}
const std::string kRequest =
    R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":-1,"topology":"18446744073709551615","requestId":"1234","operation":"2","channel":"source127"})";
void ReplaceReject(const std::string& from, const std::string& to) {
  auto input = kRequest;
  input.replace(input.find(from), from.size(), to);
  Check(!hm::preview::ParseWebPreviewRoute(input).ok(), "Unsafe route input was admitted");
}
} // namespace

int main() {
  gst_init(nullptr, nullptr);
  auto baseline = YAML::LoadFile("configs/baseline.yaml");
  auto defaults = hm::pipeline_internal::ReadPreviewOverlaySelection(baseline);
  Check(defaults.ok(), "Shared overlay baseline was rejected");
  auto explicit_defaults = YAML::Clone(baseline);
  explicit_defaults["preview_overlays"]["play"] = true;
  Check(
      hm::pipeline_internal::ReadPreviewOverlaySelection(explicit_defaults)->play,
      "Shared overlay reader ignored explicit baseline override");
  explicit_defaults["preview_overlays"].remove("rink");
  Check(
      !hm::pipeline_internal::ReadPreviewOverlaySelection(explicit_defaults).ok(),
      "Shared overlay reader supplied an independent default");
  auto configured = hm::preview::ReadWebPreviewOptions(baseline);
  const auto rink_defaults = hm::preview::ReadWebPreviewRinkMapOptions(baseline, nullptr, nullptr, nullptr);
  Check(
      rink_defaults.ok() && rink_defaults->enabled == baseline["hstream_ui"]["preview_rink_map"]["enable"].as<bool>() &&
          rink_defaults->size == baseline["hstream_ui"]["preview_rink_map"]["size"].as<std::string>() &&
          rink_defaults->position == baseline["hstream_ui"]["preview_rink_map"]["position"].as<std::string>(),
      "Mini-map preferences lost required baseline defaults");
  for (const char* size : {"icon", "compact", "standard", "large"})
    for (const char* position : {"upper-right", "upper-left", "lower-left"}) {
      const auto selected = hm::preview::ReadWebPreviewRinkMapOptions(baseline, "0", size, position);
      Check(
          selected.ok() && !selected->enabled && selected->size == size && selected->position == position,
          "Admitted mini-map preferences lost an explicit selection");
    }
  Check(
      !hm::preview::ReadWebPreviewRinkMapOptions(baseline, "true", nullptr, nullptr).ok() &&
          !hm::preview::ReadWebPreviewRinkMapOptions(baseline, nullptr, "huge", nullptr).ok() &&
          !hm::preview::ReadWebPreviewRinkMapOptions(baseline, nullptr, nullptr, "lower-right").ok() &&
          !hm::preview::ReadWebPreviewRinkMapOptions(YAML::Node(), "1", "icon", "upper-left").ok(),
      "Malformed or missing mini-map configuration was accepted");
  Check(configured.ok(), "Canonical preview baseline was rejected");
  auto changed = YAML::Clone(baseline);
  changed["preview_reference"]["program_width"] = 1440;
  const auto geometry = hm::preview::ReadPreviewReferenceGeometry(changed);
  const auto preview = hm::preview::ReadWebPreviewOptions(changed);
  Check(
      geometry.ok() && preview.ok() && geometry->program_width == 1440 &&
          preview->program_reference_width == geometry->program_width,
      "Qt and browser ignored shared preview geometry");
  changed["preview_reference"].remove("source_width");
  Check(
      !hm::preview::ReadPreviewReferenceGeometry(changed).ok() && !hm::preview::ReadWebPreviewOptions(changed).ok(),
      "Missing shared geometry used an independent default");
  changed = YAML::Clone(baseline);
  changed["web_preview"]["browser_max_frames"] = 23;
  auto custom = hm::preview::ReadWebPreviewOptions(changed);
  Check(custom.ok() && custom->browser_max_frames == 23, "Browser receipt bounds ignored resolved config");
  changed["web_preview"].remove("width");
  Check(!hm::preview::ReadWebPreviewOptions(changed).ok(), "Missing setting fell back to a compiled default");
  changed = YAML::Clone(baseline);
  changed["web_preview"]["width"] = 1599;
  Check(!hm::preview::ReadWebPreviewOptions(changed).ok(), "Odd encoder dimensions were admitted");
  changed["web_preview"]["width"] = 3840;
  changed["web_preview"]["height"] = 2160;
  changed["web_preview"]["surface_pool_frames"] = 8;
  changed["web_preview"]["conversion_pool_frames"] = 8;
  Check(!hm::preview::ReadWebPreviewOptions(changed).ok(), "Excess GPU allocation was admitted");
  changed = YAML::Clone(baseline);
  changed["web_preview"]["conversion_pool_frames"] = 3;
  Check(!hm::preview::ReadWebPreviewOptions(changed).ok(), "Encoder-starving conversion pool was admitted");
  changed = YAML::Clone(baseline);
  changed["web_preview"]["rtp_payload"] = 97;
  Check(!hm::preview::ReadWebPreviewOptions(changed).ok(), "Audio/video payload collision was admitted");
  changed = YAML::Clone(baseline);
  changed["web_preview"]["browser_max_frames"] = 513;
  Check(!hm::preview::ReadWebPreviewOptions(changed).ok(), "Browser history exceeds native receipt retention");

  auto request = hm::preview::ParseWebPreviewRoute(kRequest);
  const auto rotation_values = hm::program::ReadRotationControls(baseline);
  Check(rotation_values.ok(), "Native rotation baseline was rejected");
  const std::string rotation_json =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":0,"topology":"9007199254740993","requestId":"1234","controls":{"Stitch_Rotate_Degrees":73,"Left_Fixed_Edge_Rotation_Angle_x10":127}})";
  const auto rotation = hm::preview::ParseWebRotationControls(rotation_json, *rotation_values);
  Check(rotation.ok() && rotation->controls.at("Stitch_Rotate_Degrees") == 73 &&
            rotation->session.topology == 9007199254740993ULL,
        "Typed rotation changed native units or topology precision");
  for (const auto& [before, after] : {
           std::pair{"\"Stitch_Rotate_Degrees\":73", "\"Stitch_Rotate_Degrees\":73.5"},
           std::pair{"\"Stitch_Rotate_Degrees\":73", "\"Stitch_Rotate_Degrees\":181"},
           std::pair{"\"Left_Fixed_Edge_Rotation_Angle_x10\":127", "\"Left_Fixed_Edge_Rotation_Angle_x10\":true"},
           std::pair{"\"Left_Fixed_Edge_Rotation_Angle_x10\":127", "\"unknown\":127"},
           std::pair{"\"version\":1", "\"version\":1,\"version\":1"},
           std::pair{"\"requestId\":\"1234\"", "\"requestId\":1234"}}) {
    auto unsafe = rotation_json;
    unsafe.replace(unsafe.find(before), std::strlen(before), after);
    Check(!hm::preview::ParseWebRotationControls(unsafe, *rotation_values).ok(),
          "Invalid typed rotation escaped validation");
  }
  const std::string rotation_query =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":0,"topology":"9007199254740993","requestId":"1234"})";
  Check(hm::preview::ParseWebRotationControls(rotation_query, *rotation_values, true).ok() &&
            !hm::preview::ParseWebRotationControls(rotation_query, *rotation_values).ok() &&
            !hm::preview::ParseWebRotationControls(rotation_json, *rotation_values, true).ok(),
        "Rotation query admitted mutation fields");
  Check(hm::preview::WebRotationControlsReply(*rotation, "authorizing", "", *rotation_values)
                .find("\"operation\":null") != std::string::npos &&
            hm::preview::WebRotationControlsReply(*rotation, "applied", "", *rotation_values, 9007199254740994ULL)
                .find("\"operation\":\"9007199254740994\"") != std::string::npos,
        "Rotation phases did not distinguish authorization from exact presentation operation");
  Check(
      request.ok() && request->session.topology == UINT64_MAX && request->session.stage == -1 &&
          request->request_id == "1234",
      "Route identity lost signed stage or uint64 precision");
  ReplaceReject("\"operation\":\"2\"", "\"operation\":2");
  ReplaceReject("\"operation\":\"2\"", "\"operation\":\"02\"");
  ReplaceReject("source127", "source128");
  ReplaceReject("source127", "source01");
  ReplaceReject("\"stage\":-1", "\"stage\":1.5");
  ReplaceReject("\"1234\"", "\"bad id\"");
  ReplaceReject("\"1234\"", "\"" + std::string(65, '1') + "\"");
  ReplaceReject("18446744073709551615", "18446744073709551616");
  ReplaceReject("\"version\":1", "\"version\":2");
  Check(!hm::preview::ParseWebPreviewRoute(std::string(1025, ' ')).ok(), "Oversize command was admitted");

  hm::preview::RouteOwner owner;
  owner.bind_session(request->session);
  owner.initialize_selection("none", 1);
  bool active = false;
  owner.register_route(
      "source127",
      {
          [&](bool value, uint64_t) { active = value; },
          [](bool, uint64_t) {},
          [&] { return active; },
          [](uint64_t) {},
          [](uint64_t) { return true; },
          [](uint64_t) { return true; },
      });
  auto stale = *request;
  ++stale.session.stage;
  Check(
      owner.select(stale, {}).status == hm::preview::RouteOwner::Status::kWrongSession && !active,
      "Stale session mutated native routes");
  auto result = owner.select(*request, {});
  Check(result.status == hm::preview::RouteOwner::Status::kActivated && active, "Valid selection was rejected");
  auto* pipeline = gst_pipeline_new("pause-admission");
  auto deferred = *request;
  deferred.channel = "none";
  deferred.operation = 3;
  unsigned changed_routes = 0;
  const auto changed_route = [&] { ++changed_routes; };
  for (const auto desired_state : {GST_STATE_NULL, GST_STATE_READY, GST_STATE_PAUSED}) {
    gst_element_set_state(pipeline, desired_state);
    const auto paused_result = hm::preview::SelectWebPreviewRoute(pipeline, owner, deferred, changed_route);
    Check(
        paused_result.status == hm::preview::RouteOwner::Status::kFailed && changed_routes == 0 && active &&
            owner.operation_generation() == 2,
        "Nonplaying route request entered a drain or changed route identity");
    if (desired_state == GST_STATE_PAUSED)
      Check(paused_result.reason == "preview-paused", "Paused route rejection lost its reason");
  }
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  auto wrong_stage = deferred;
  ++wrong_stage.session.stage;
  Check(
      hm::preview::SelectWebPreviewRoute(pipeline, owner, wrong_stage, changed_route).status ==
              hm::preview::RouteOwner::Status::kWrongSession &&
          changed_routes == 0 && active,
      "Resuming admitted a deferred request from the old stage");
  const auto resumed = hm::preview::SelectWebPreviewRoute(pipeline, owner, deferred, changed_route);
  Check(
      resumed.status == hm::preview::RouteOwner::Status::kDeactivated && changed_routes == 1 && !active,
      "Playing route did not apply after resuming");
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  // Restore the original route for the capability serialization checks below.
  auto restored = *request;
  restored.operation = 4;
  Check(
      owner.select(restored, {}).status == hm::preview::RouteOwner::Status::kActivated,
      "Could not restore capability route after pause checks");
  const auto reply = hm::preview::WebPreviewRouteReply(*request, result);
  Check(
      reply.find("\"requestId\":\"1234\"") != std::string::npos &&
          reply.find("\"status\":\"activated\"") != std::string::npos,
      "Reply lost request correlation");
  auto* parser = json_parser_new();
  const auto capability =
      hm::preview::WebPreviewCapability(*custom, owner, hm::pipeline_internal::PreviewOverlayRuntimeState(*defaults));
  Check(
      json_parser_load_from_data(parser, capability.data(), capability.size(), nullptr), "Capability is invalid JSON");
  auto* object = json_node_get_object(json_parser_get_root(parser));
  auto* limits = json_object_get_object_member(object, "receiptLimits");
  Check(
      json_object_get_int_member(limits, "maxFrames") == 23 && !json_object_get_boolean_member(object, "seek") &&
          json_object_get_int_member(object, "rinkMiniMapVersion") == 1 &&
          std::string(json_object_get_string_member(object, "channel")) == "source127",
      "Capability did not reflect resolved limits and active route");
  const auto seek_capability = hm::preview::WebPreviewCapability(
      *custom, owner, hm::pipeline_internal::PreviewOverlayRuntimeState(*defaults), true);
  Check(
      json_parser_load_from_data(parser, seek_capability.data(), seek_capability.size(), nullptr) &&
          json_object_get_boolean_member(json_node_get_object(json_parser_get_root(parser)), "seek"),
      "Seekable session lost its capability");
  g_object_unref(parser);

  const std::string scoreboard_json =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":0,"topology":"7","requestId":"1234","action":"position","position":"upper-center"})";
  const auto scoreboard = hm::preview::ParseWebScoreboard(scoreboard_json);
  Check(scoreboard.ok() && scoreboard->position == "upper-center", "Scoreboard placement was lost");
  for (const auto& change :
       {std::pair<std::string, std::string>{"upper-center", "lower-left"},
        {"\"stage\":0", "\"stage\":false"},
        {"\"requestId\":\"1234\"", "\"requestId\":1234"},
        {"\"version\":1", "\"version\":1,\"version\":1"},
        {"\"action\":\"position\"", "\"action\":\"reselect\""}}) {
    auto invalid = scoreboard_json;
    invalid.replace(invalid.find(change.first), change.first.size(), change.second);
    Check(!hm::preview::ParseWebScoreboard(invalid).ok(), "Unsafe scoreboard request was admitted");
  }
  auto scoreboard_query = scoreboard_json.substr(0, scoreboard_json.find(",\"action\""));
  Check(
      hm::preview::ParseWebScoreboard(scoreboard_query + R"(,"action":"query"})").ok(),
      "Scoreboard query was rejected");
  Check(
      hm::preview::ParseWebScoreboard(scoreboard_query + R"(,"action":"reselect"})").ok(),
      "Scoreboard reselection was rejected");
  const auto scoreboard_reply = hm::preview::WebScoreboardReply(*scoreboard, "applied", "", "upper-center");
  Check(
      scoreboard_reply.find("\"supported\":true") != std::string::npos &&
          scoreboard_reply.find("\"position\":\"upper-center\"") != std::string::npos,
      "Scoreboard response lost support or native placement");
  Check(
      hm::preview::WebScoreboardReply(*scoreboard, "failed", "stale-session", std::nullopt).find("\"position\":null") !=
          std::string::npos,
      "Unavailable scoreboard response invented a placement");

  const auto camera_defaults = hm::tracking::ReadPlaytrackerControlValues(baseline);
  Check(camera_defaults.ok(), "Required camera control baseline was rejected");
  const std::string camera_json =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":0,"topology":"7","requestId":"1234","forceAllTargets":false,"controls":{"Max_Speed_X_x10":123,"Oversized_Player_Percent":125.25,"Cancel_Stop_On_Opposite_Direction":1}})";
  const auto camera = hm::preview::ParseWebCameraControls(camera_json, *camera_defaults);
  Check(
      camera.ok() && !camera->force_all_targets && camera->controls.size() == 3 &&
          camera->controls.at("Oversized_Player_Percent") == 125.25,
      "Camera controls lost identity or fractional percentage");
  for (const auto& replacement :
       {std::make_pair("\"Max_Speed_X_x10\":123", "\"Max_Speed_X_x10\":2001"),
        std::make_pair("\"Max_Speed_X_x10\":123", "\"Max_Speed_X_x10\":1.5"),
        std::make_pair("\"Max_Speed_X_x10\":123", "\"Max_Speed_X_x10\":true"),
        std::make_pair("\"Max_Speed_X_x10\":123", "\"unknown\":123"),
        std::make_pair("\"Max_Speed_X_x10\":123", "\"Max_Speed_X_x10\":123,\"Max_Speed_X_x10\":0"),
        std::make_pair("\"Oversized_Player_Percent\":125.25", "\"Oversized_Player_Percent\":-1"),
        std::make_pair("\"requestId\":\"1234\"", "\"requestId\":\"01234\""),
        std::make_pair("\"forceAllTargets\":false", "\"forceAllTargets\":1"),
        std::make_pair("\"forceAllTargets\":false", "\"forceAllTargets\":false,\"forceAllTargets\":true"),
        std::make_pair("\"forceAllTargets\":false,", "")}) {
    auto invalid = camera_json;
    invalid.replace(invalid.find(replacement.first), std::strlen(replacement.first), replacement.second);
    Check(!hm::preview::ParseWebCameraControls(invalid, *camera_defaults).ok(), "Unsafe camera controls were admitted");
  }
  auto camera_reset = camera_json;
  camera_reset.replace(camera_reset.find("\"forceAllTargets\":false"), 23, "\"forceAllTargets\":true");
  const auto reset_request = hm::preview::ParseWebCameraControls(camera_reset, *camera_defaults);
  Check(reset_request.ok() && reset_request->force_all_targets, "Camera Reset lost all-target publication");
  auto camera_query = camera_json;
  camera_query.erase(camera_query.find(",\"forceAllTargets\""));
  camera_query += "}";
  Check(
      hm::preview::ParseWebCameraControls(camera_query, *camera_defaults, true).ok() &&
          !hm::preview::ParseWebCameraControls(camera_json, *camera_defaults, true).ok() &&
          !hm::preview::ParseWebCameraControls(camera_query, *camera_defaults).ok(),
      "Camera mutation and readback envelopes were interchangeable");
  const auto camera_reply = hm::preview::WebCameraControlsReply(*camera, "applied", "", camera->controls);
  Check(
      camera_reply.find("125.25") != std::string::npos &&
          camera_reply.find("\"requestId\":\"1234\"") != std::string::npos,
      "Camera reply lost exact readback or request correlation");

  const std::string drawing_json =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":0,"topology":"7","requestId":"1234","layers":31})";
  const auto drawing = hm::preview::ParseWebPlayerDrawing(drawing_json);
  Check(
      drawing.ok() && drawing->layers == 31 && drawing->session.topology == 7,
      "Player drawing identity or full selection was lost");
  for (const auto& replacement :
       {std::make_pair("\"layers\":31", "\"layers\":32"),
        std::make_pair("\"layers\":31", "\"layers\":true"),
        std::make_pair("\"layers\":31", "\"layers\":1.5"),
        std::make_pair("\"requestId\":\"1234\"", "\"requestId\":\"0\""),
        std::make_pair("\"layers\":31", "\"layers\":31,\"layers\":0"),
        std::make_pair("\"topology\":\"7\"", "\"topology\":\"07\"")}) {
    auto invalid = drawing_json;
    invalid.replace(invalid.find(replacement.first), std::strlen(replacement.first), replacement.second);
    Check(!hm::preview::ParseWebPlayerDrawing(invalid).ok(), "Unsafe player drawing request was admitted");
  }
  auto drawing_query = drawing_json;
  drawing_query.erase(drawing_query.find(",\"layers\""));
  drawing_query += "}";
  Check(
      hm::preview::ParseWebPlayerDrawing(drawing_query, true).ok() &&
          !hm::preview::ParseWebPlayerDrawing(drawing_query).ok() &&
          !hm::preview::ParseWebPlayerDrawing(drawing_json, true).ok(),
      "Player drawing query and mutation envelopes were interchangeable");
  const auto drawing_reply = hm::preview::WebPlayerDrawingReply(*drawing, "applied", "", 31);
  Check(
      drawing_reply.find("\"layers\":31") != std::string::npos &&
          drawing_reply.find("\"requestId\":\"1234\"") != std::string::npos,
      "Player drawing reply lost readback or correlation");

  const std::string seek_json =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":0,"topology":"7","requestId":"1234","positionNs":"9223372036854775807"})";
  const auto seek = hm::preview::ParseWebPreviewSeek(seek_json);
  Check(
      seek.ok() && seek->position_ns == INT64_MAX && seek->request_id == 1234 && seek->session.topology == 7,
      "Seek identity or nanosecond time lost precision");
  for (const auto& invalid :
       {std::pair{std::string("\"version\":1"), std::string("\"version\":true")},
        {"\"stage\":0", "\"stage\":0.0"},
        {"\"topology\":\"7\"", "\"topology\":7"},
        {"\"topology\":\"7\"", "\"topology\":\"0\""},
        {"\"requestId\":\"1234\"", "\"requestId\":\"0\""},
        {"\"requestId\":\"1234\"", "\"requestId\":\"9223372036854775808\""},
        {"\"positionNs\":\"9223372036854775807\"", "\"positionNs\":\"9223372036854775808\""},
        {"\"positionNs\":\"9223372036854775807\"", "\"positionNs\":\"-1\""},
        {"\"positionNs\":\"9223372036854775807\"", "\"positionNs\":\"01\""},
        {"\"positionNs\":\"9223372036854775807\"", "\"positionNs\":1"},
        {"\"version\":1", "\"version\":1,\"version\":1"},
        {"\"stage\":0", "\"stage\":0,\"extra\":0"}}) {
    auto input = seek_json;
    input.replace(input.find(invalid.first), invalid.first.size(), invalid.second);
    Check(!hm::preview::ParseWebPreviewSeek(input).ok(), "Malformed seek envelope admitted");
  }
  auto zero_seek = seek_json;
  zero_seek.replace(zero_seek.find("9223372036854775807"), 19, "0");
  Check(hm::preview::ParseWebPreviewSeek(zero_seek).ok(), "Seek to start rejected");
  auto relative_seek = zero_seek;
  relative_seek.replace(relative_seek.find("\"positionNs\":\"0\""), 16, "\"deltaNs\":\"-10000000000\"");
  const auto relative = hm::preview::ParseWebPreviewSeek(relative_seek);
  Check(relative.ok() && relative->delta_ns == -10000000000LL, "Relative seek lost its signed native offset");
  for (const auto& delta : {"0", "-0", "01", "+1", "-01", "9223372036854775808", "-9223372036854775809"}) {
    auto input = relative_seek;
    input.replace(input.find("-10000000000"), 12, delta);
    Check(!hm::preview::ParseWebPreviewSeek(input).ok(), "Noncanonical relative seek admitted");
  }
  Check(!hm::preview::ParseWebPreviewSeek(std::string(4097, ' ')).ok(), "Oversize seek admitted");
  const auto completed_seek = hm::preview::WebPreviewSeekReply(*seek, "completed", "", 8, 4000000000ULL, 0);
  Check(
      completed_seek.find("\"resultTopology\":\"8\"") != std::string::npos &&
          completed_seek.find("\"positionNs\":\"4000000000\"") != std::string::npos &&
          completed_seek.find("\"sourcePtsNs\":\"0\"") != std::string::npos,
      "Seek reply lost replacement topology or first completed frame");
  Check(
      hm::preview::WebPreviewSeekReply(*seek, "accepted", "", 7).find("\"sourcePtsNs\":null") != std::string::npos,
      "Seek admission invented first-frame evidence");

  const std::string overlay_json =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":-1,"topology":"18446744073709551615","requestId":"overlay-1","generation":"18446744073709551615","players":true,"play":false,"rink":true})";
  auto overlay = hm::preview::ParseWebPreviewOverlays(overlay_json);
  Check(overlay.ok() && overlay->command.generation == UINT64_MAX, "Overlay uint64 identity lost precision");
  for (const auto& invalid :
       {std::pair{std::string("true"), std::string("1")},
        {"\"generation\":\"18446744073709551615\"", "\"generation\":1"},
        {"\"generation\":\"18446744073709551615\"", "\"generation\":\"0\""},
        {"\"rink\":true", "\"rink\":true,\"extra\":false"},
        {"\"rink\":true", "\"rink\":false,\"rink\":true"},
        {"\"play\":false,", ""},
        {"overlay-1", "bad request"}}) {
    auto input = overlay_json;
    input.replace(input.find(invalid.first), invalid.first.size(), invalid.second);
    Check(!hm::preview::ParseWebPreviewOverlays(input).ok(), "Malformed overlay envelope admitted");
  }
  hm::pipeline_internal::PreviewOverlayRuntimeState state(*defaults);
  unsigned mutations = 0;
  const auto apply = [&](const auto&) {
    ++mutations;
    return true;
  };
  auto wrong = *overlay;
  ++wrong.session.stage;
  const auto stale_reply = hm::preview::ApplyWebPreviewOverlays(wrong, overlay->session, state, "", apply);
  Check(
      stale_reply.find("\"status\":\"stale\"") != std::string::npos && mutations == 0 && state.generation() == 0,
      "Stale overlay session mutated state");
  const auto blocked = hm::preview::ApplyWebPreviewOverlays(*overlay, overlay->session, state, "preview-paused", apply);
  Check(
      blocked.find("preview-paused") != std::string::npos && mutations == 0 && state.generation() == 0,
      "Paused overlay request entered the mutation or advanced its generation");
  const auto failed =
      hm::preview::ApplyWebPreviewOverlays(*overlay, overlay->session, state, "", [](const auto&) { return false; });
  Check(
      failed.find("\"status\":\"failed\"") != std::string::npos && state.generation() == 0 &&
          state.selection() == *defaults,
      "Failed overlay transaction changed committed selection");
  const auto applied = hm::preview::ApplyWebPreviewOverlays(*overlay, overlay->session, state, "", apply);
  Check(
      applied.find("\"status\":\"applied\"") != std::string::npos && mutations == 1 &&
          state.generation() == UINT64_MAX && state.selection() == overlay->command.selection,
      "Overlay commit lost authoritative selection");
  hm::preview::ApplyWebPreviewOverlays(*overlay, overlay->session, state, "", apply);
  Check(mutations == 1, "Replayed overlay request mutated state");
  const std::string query_json =
      R"({"version":1,"run":"01234567-89ab-cdef-0123-456789abcdef","stage":-1,"topology":"18446744073709551615","requestId":"query-1"})";
  const auto query = hm::preview::ParseWebPreviewOverlays(query_json, true);
  Check(
      query.ok() && !hm::preview::ParseWebPreviewOverlays(query_json).ok() &&
          !hm::preview::ParseWebPreviewOverlays(overlay_json, true).ok(),
      "Query/mutation envelope was interchangeable");
  const auto queried = hm::preview::ApplyWebPreviewOverlays(*query, overlay->session, state, "preview-paused", apply);
  Check(
      queried.find("\"status\":\"state\"") != std::string::npos &&
          queried.find("\"generation\":\"18446744073709551615\"") != std::string::npos && mutations == 1,
      "Read-only overlay query changed state or lost full readback");
  const auto artifact = hm::preview::WebPreviewOverlayArtifactEvent(
      {overlay->session, UINT64_MAX, 0, "stitched", "unavailable", "Rink mask overlay unavailable"});
  parser = json_parser_new();
  Check(json_parser_load_from_data(parser, artifact.data(), artifact.size(), nullptr), "Invalid artifact event JSON");
  object = json_node_get_object(json_parser_get_root(parser));
  Check(
      json_object_get_size(object) == 10 &&
          std::string(json_object_get_string_member(object, "operation")) == "18446744073709551615" &&
          std::string(json_object_get_string_member(object, "generation")) == "0" &&
          std::string(json_object_get_string_member(object, "artifact")) == "rink-mask" &&
          std::string(json_object_get_string_member(object, "channel")) == "stitched",
      "Artifact event lost route/selection identity or initial generation");
  g_object_unref(parser);
  // Losing the mutation acknowledgment can discard the first artifact status
  // at the outer bridge. Query recovery must republish it even with no new frame.
  hm::preview::WebPreviewOverlayArtifactMailbox mailbox;
  const hm::preview::WebPreviewOverlayArtifact unavailable{
      overlay->session, 2, 1, "stitched", "unavailable", "Rink mask overlay unavailable"};
  hm::pipeline_internal::PreviewOverlayCommand selected{1, {false, false, true}};
  const auto take = [&]() { return mailbox.Take(overlay->session, 2, "stitched", selected); };
  mailbox.Publish(unavailable);
  Check(take().has_value(), "First mask status was lost");
  mailbox.Publish(unavailable);
  Check(!take(), "Unchanged painted frames duplicated mask status");
  mailbox.Replay(overlay->session);
  const auto recovered = take();
  Check(recovered && recovered->reason == unavailable.reason, "Query could not recover unchanged unavailable mask");
  auto retired_session = overlay->session;
  retired_session.topology = 1;
  mailbox.Replay(retired_session);
  Check(!take(), "Stale query republished mask status");
  mailbox.Replay(overlay->session);
  Check(!mailbox.Take(retired_session, 2, "stitched", selected), "Retired topology exposed old mask status");
  mailbox.Replay(overlay->session);
  Check(!mailbox.Take(overlay->session, 3, "stitched", selected), "New route operation exposed old mask status");
  mailbox.Replay(overlay->session);
  Check(!mailbox.Take(overlay->session, 2, "program", selected), "New channel exposed old mask status");
  mailbox.Replay(overlay->session);
  selected.generation = 2;
  Check(!take(), "New overlay generation exposed old mask status");
  selected.generation = 1;
  selected.selection.rink = false;
  mailbox.Replay(overlay->session);
  Check(!take(), "Disabled rink overlay exposed old mask status");
  selected.selection.rink = true;
  auto ready = unavailable;
  ready.status = "ready";
  ready.reason.clear();
  mailbox.Publish(ready);
  Check(take()->status == "ready", "Recovery did not replace unavailable mask status");
  mailbox.Replay(overlay->session);
  Check(take()->status == "ready", "Query replayed an older unavailable status");
  // The next-Play mini-map is independent of the live mask checkbox and epoch,
  // while retaining the same exact session/route fences and bounded mailbox.
  hm::preview::WebPreviewOverlayArtifactMailbox mini_map;
  std::string private_error(65536, 'x');
  private_error += "/private/worker/calibration\n";
  private_error.append("\0\xff", 2);
  const auto failed_projection = absl::InternalError(private_error);
  const auto public_reason = hm::preview::WebPreviewRinkMapFailureReason(failed_projection.code());
  Check(
      !public_reason.empty() && public_reason.size() <= 1024 &&
          public_reason.find("/private/") == std::string::npos &&
          public_reason.find('\0') == std::string::npos && g_utf8_validate(public_reason.c_str(), -1, nullptr),
      "Projection diagnostics escaped into the bounded public mini-map reason");
  Check(
      hm::preview::WebPreviewRinkMapFailureReason(absl::StatusCode::kOk).empty() &&
          hm::preview::WebPreviewRinkMapFailureReason(absl::StatusCode::kFailedPrecondition).find("save") !=
              std::string::npos &&
          hm::preview::WebPreviewRinkMapFailureReason(absl::StatusCode::kNotFound).find("diagnostics") !=
              std::string::npos,
      "Mini-map status lost its ready/required-review distinction");
  const hm::preview::WebPreviewOverlayArtifact mini_ready{
      overlay->session, 2, 0, "stitched", "ready", "", "rink-mini-map"};
  mini_map.Publish(mini_ready);
  const auto mini_event = mini_map.Take(overlay->session, 2, "stitched", 0, true);
  Check(
      mini_event && mini_event->artifact == "rink-mini-map" &&
          hm::preview::WebPreviewOverlayArtifactEvent(*mini_event).find("\"artifact\":\"rink-mini-map\"") !=
              std::string::npos,
      "Mini-map status became a mask status");
  for (const auto& candidate : {overlay->session, retired_session}) {
    mini_map.Replay(overlay->session);
    Check(!mini_map.Take(candidate, 3, "stitched", 0, true), "A new operation admitted an old mini-map frame");
  }
  mini_map.Replay(overlay->session);
  Check(!mini_map.Take(retired_session, 2, "stitched", 0, true), "Retired session exposed a mini-map frame");
  mini_map.Replay(overlay->session);
  Check(!mini_map.Take(overlay->session, 2, "program", 0, true), "New channel exposed a mini-map frame");
  mini_map.Replay(overlay->session);
  Check(!mini_map.Take(overlay->session, 2, "stitched", 0, false), "Disabled mini-map exposed a frame");
  mini_map.Replay(overlay->session);
  Check(!mini_map.Take(overlay->session, 2, "stitched", 1, true), "Mini-map accepted a mutable overlay epoch");
  mini_map.Replay(overlay->session);
  Check(mini_map.Take(overlay->session, 2, "stitched", 0, true).has_value(), "Mini-map query replay lost readiness");
  return 0;
}
