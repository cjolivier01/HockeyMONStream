#include "WebPreviewOptions.h"
#include "WebPreviewRuntime.h"
#include "hstream/src/libs/program/PreviewOverlayConfig.h"

#include <json-glib/json-glib.h>

#include <cstdlib>
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
          std::string(json_object_get_string_member(object, "channel")) == "source127",
      "Capability did not reflect resolved limits and active route");
  g_object_unref(parser);

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
  return 0;
}
