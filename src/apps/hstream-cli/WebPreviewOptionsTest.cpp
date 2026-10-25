#include "WebPreviewOptions.h"

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
  auto baseline = YAML::LoadFile("configs/baseline.yaml");
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
  const auto reply = hm::preview::WebPreviewRouteReply(*request, result);
  Check(
      reply.find("\"requestId\":\"1234\"") != std::string::npos &&
          reply.find("\"status\":\"activated\"") != std::string::npos,
      "Reply lost request correlation");
  auto* parser = json_parser_new();
  const auto capability = hm::preview::WebPreviewCapability(*custom, owner);
  Check(
      json_parser_load_from_data(parser, capability.data(), capability.size(), nullptr), "Capability is invalid JSON");
  auto* object = json_node_get_object(json_parser_get_root(parser));
  auto* limits = json_object_get_object_member(object, "receiptLimits");
  Check(
      json_object_get_int_member(limits, "maxFrames") == 23 && !json_object_get_boolean_member(object, "seek") &&
          std::string(json_object_get_string_member(object, "channel")) == "source127",
      "Capability did not reflect resolved limits and active route");
  g_object_unref(parser);
  return 0;
}
