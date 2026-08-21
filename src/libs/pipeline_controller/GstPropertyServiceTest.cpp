#include "hstream/src/libs/pipeline_controller/GstPropertyService.h"

#include <gst/gst.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct RejectingTestElement {
  GstElement parent;
  gdouble level;
  gfloat float_level;
  gboolean last_property_set_ok;
  guint sensitive_get_count;
};

struct RejectingTestElementClass {
  GstElementClass parent_class;
};

enum RejectingTestProperty {
  kRejectingPropertyNone,
  kRejectingPropertyLevel,
  kRejectingPropertyFloatLevel,
  kRejectingPropertyLastSetOk,
  kRejectingPropertyUserPassword,
  kRejectingPropertyExtraHeaders,
  kRejectingPropertyCookies,
  kRejectingPropertyClientKey,
};

G_DEFINE_TYPE(RejectingTestElement, rejecting_test_element, GST_TYPE_ELEMENT)

void rejecting_test_element_set_property(GObject* object, guint property_id, const GValue* value, GParamSpec* pspec) {
  auto* element = reinterpret_cast<RejectingTestElement*>(object);
  switch (property_id) {
    case kRejectingPropertyLevel: {
      const gdouble requested = g_value_get_double(value);
      element->last_property_set_ok = std::isfinite(requested) && requested <= 0.5;
      if (element->last_property_set_ok) {
        element->level = requested;
      }
      return;
    }
    case kRejectingPropertyFloatLevel: {
      const gfloat requested = g_value_get_float(value);
      element->last_property_set_ok = std::isfinite(requested) && requested <= 0.5F;
      if (element->last_property_set_ok) {
        element->float_level = requested;
      }
      return;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
  }
}

void rejecting_test_element_get_property(GObject* object, guint property_id, GValue* value, GParamSpec* pspec) {
  auto* element = reinterpret_cast<RejectingTestElement*>(object);
  switch (property_id) {
    case kRejectingPropertyLevel:
      g_value_set_double(value, element->level);
      return;
    case kRejectingPropertyFloatLevel:
      g_value_set_float(value, element->float_level);
      return;
    case kRejectingPropertyLastSetOk:
      g_value_set_boolean(value, element->last_property_set_ok);
      return;
    case kRejectingPropertyUserPassword:
      ++element->sensitive_get_count;
      g_value_set_string(value, "current-user-password");
      return;
    case kRejectingPropertyExtraHeaders:
      ++element->sensitive_get_count;
      g_value_set_string(value, "Authorization: Bearer current-token");
      return;
    case kRejectingPropertyCookies:
      ++element->sensitive_get_count;
      g_value_set_string(value, "session=current-cookie");
      return;
    case kRejectingPropertyClientKey:
      ++element->sensitive_get_count;
      g_value_set_string(value, "current-private-key");
      return;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
  }
}

void rejecting_test_element_class_init(RejectingTestElementClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->set_property = rejecting_test_element_set_property;
  object_class->get_property = rejecting_test_element_get_property;
  const auto live_flags = static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING);
  g_object_class_install_property(
      object_class,
      kRejectingPropertyLevel,
      g_param_spec_double("level", "Level", "Reject values above 0.5", 0.0, 1.0, 0.0, live_flags));
  g_object_class_install_property(
      object_class,
      kRejectingPropertyFloatLevel,
      g_param_spec_float("float-level", "Float level", "Reject values above 0.5", 0.0F, 1.0F, 0.0F, live_flags));
  g_object_class_install_property(
      object_class,
      kRejectingPropertyLastSetOk,
      g_param_spec_boolean(
          "last-property-set-ok",
          "Last property set OK",
          "Whether the plugin accepted its last value",
          TRUE,
          G_PARAM_READABLE));
  g_object_class_install_property(
      object_class,
      kRejectingPropertyUserPassword,
      g_param_spec_string(
          "user-pw", "User password", "Authentication password", "default-user-password", G_PARAM_READABLE));
  g_object_class_install_property(
      object_class,
      kRejectingPropertyExtraHeaders,
      g_param_spec_string(
          "extra-headers",
          "Extra headers",
          "HTTP request headers",
          "Authorization: Bearer default-token",
          G_PARAM_READABLE));
  g_object_class_install_property(
      object_class,
      kRejectingPropertyCookies,
      g_param_spec_string("cookies", "Cookies", "HTTP cookies", "session=default-cookie", G_PARAM_READABLE));
  g_object_class_install_property(
      object_class,
      kRejectingPropertyClientKey,
      g_param_spec_string("client-key", "Client key", "TLS client key", "default-private-key", G_PARAM_READABLE));
}

void rejecting_test_element_init(RejectingTestElement* element) {
  element->level = 0.0;
  element->float_level = 0.0F;
  element->last_property_set_ok = TRUE;
  element->sensitive_get_count = 0;
}

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool test_list_and_set_identity_properties() {
  GstElement* identity = gst_element_factory_make("identity", "identity_for_properties");
  if (!expect(identity != nullptr, "Failed to create identity element")) {
    return false;
  }

  auto properties_or = hm::pipeline::listElementProperties(identity);
  if (!expect(properties_or.ok(), properties_or.status().ToString())) {
    gst_object_unref(identity);
    return false;
  }

  bool found_silent = false;
  bool found_sleep_time = false;
  bool found_name = false;
  for (const hm::pipeline::GstPropertyInfo& property : *properties_or) {
    if (property.name == "silent") {
      found_silent = true;
      if (!expect(property.readable, "identity.silent should be readable") ||
          !expect(property.writable, "identity.silent should be writable") ||
          !expect(property.runtime_writable, "identity.silent should be runtime writable while stopped") ||
          !expect(!property.live_writable, "identity.silent should not be live writable") ||
          !expect(property.advanced, "identity.silent should be advanced because it requires restart") ||
          !expect(
              property.control_kind == hm::pipeline::RuntimeControlKind::Toggle,
              "identity.silent should be a toggle")) {
        gst_object_unref(identity);
        return false;
      }
    } else if (property.name == "sleep-time") {
      found_sleep_time = true;
      if (!expect(
              property.control_kind == hm::pipeline::RuntimeControlKind::Integer,
              "identity.sleep-time should be an integer control") ||
          !expect(property.minimum_value == "0", "identity.sleep-time should expose minimum") ||
          !expect(property.maximum_value == "4294967295", "identity.sleep-time should expose maximum") ||
          !expect(property.default_value == "0", "identity.sleep-time should expose default")) {
        gst_object_unref(identity);
        return false;
      }
    } else if (property.name == "name") {
      found_name = true;
      if (!expect(property.secret, "string properties must fail closed during inspection") ||
          !expect(property.serialized_value == "[redacted]", "string current values must be redacted") ||
          !expect(property.default_value == "[redacted]", "string default values must be redacted")) {
        gst_object_unref(identity);
        return false;
      }
    }
  }
  if (!expect(found_silent, "identity.silent was not listed")) {
    gst_object_unref(identity);
    return false;
  }
  if (!expect(found_sleep_time, "identity.sleep-time was not listed")) {
    gst_object_unref(identity);
    return false;
  }
  if (!expect(found_name, "identity.name was not listed")) {
    gst_object_unref(identity);
    return false;
  }

  absl::Status set_status = hm::pipeline::setElementPropertyFromString(identity, "silent", "false");
  if (!expect(set_status.ok(), set_status.ToString())) {
    gst_object_unref(identity);
    return false;
  }

  gst_element_set_state(identity, GST_STATE_PLAYING);
  gst_element_get_state(identity, nullptr, nullptr, GST_SECOND);
  absl::Status playing_set_status = hm::pipeline::setElementPropertyFromString(identity, "silent", "true");
  if (!expect(!playing_set_status.ok(), "identity.silent should not be mutable while PLAYING without GST mutability")) {
    gst_element_set_state(identity, GST_STATE_NULL);
    gst_object_unref(identity);
    return false;
  }
  gst_element_set_state(identity, GST_STATE_NULL);

  gboolean silent = TRUE;
  g_object_get(G_OBJECT(identity), "silent", &silent, nullptr);
  if (!expect(!silent, "identity.silent was not updated")) {
    gst_object_unref(identity);
    return false;
  }

  gst_object_unref(identity);
  return true;
}

bool test_enum_metadata_and_parsing() {
  GstElement* videotestsrc = gst_element_factory_make("videotestsrc", "source_for_enum_properties");
  if (!expect(videotestsrc != nullptr, "Failed to create videotestsrc element")) {
    return false;
  }

  auto properties_or = hm::pipeline::listElementProperties(videotestsrc);
  if (!expect(properties_or.ok(), properties_or.status().ToString())) {
    gst_object_unref(videotestsrc);
    return false;
  }

  bool found_pattern = false;
  for (const hm::pipeline::GstPropertyInfo& property : *properties_or) {
    if (property.name == "pattern") {
      found_pattern = true;
      if (!expect(
              property.control_kind == hm::pipeline::RuntimeControlKind::Enum,
              "videotestsrc.pattern should be an enum control") ||
          !expect(!property.enum_values.empty(), "videotestsrc.pattern should expose enum values")) {
        gst_object_unref(videotestsrc);
        return false;
      }
      break;
    }
  }
  if (!expect(found_pattern, "videotestsrc.pattern was not listed")) {
    gst_object_unref(videotestsrc);
    return false;
  }

  absl::Status set_status = hm::pipeline::setElementPropertyFromString(videotestsrc, "pattern", "snow");
  if (!expect(set_status.ok(), set_status.ToString())) {
    gst_object_unref(videotestsrc);
    return false;
  }

  gst_object_unref(videotestsrc);
  return true;
}

bool test_invalid_values_are_rejected() {
  GstElement* identity = gst_element_factory_make("identity", "identity_for_invalid_properties");
  if (!expect(identity != nullptr, "Failed to create identity element for invalid values")) {
    return false;
  }

  absl::Status negative_unsigned = hm::pipeline::setElementPropertyFromString(identity, "sleep-time", "-1");
  absl::Status trailing_chars = hm::pipeline::setElementPropertyFromString(identity, "silent", "1junk");
  if (!expect(!negative_unsigned.ok(), "negative unsigned integer should be rejected") ||
      !expect(!trailing_chars.ok(), "boolean value with trailing characters should be rejected")) {
    gst_object_unref(identity);
    return false;
  }

  gst_object_unref(identity);
  return true;
}

bool test_non_finite_and_plugin_rejected_values_are_rejected() {
  GstElement* element = GST_ELEMENT(g_object_new(rejecting_test_element_get_type(), nullptr));
  if (!expect(element != nullptr, "Failed to create rejecting test element")) {
    return false;
  }

  const absl::Status finite = hm::pipeline::setElementPropertyFromString(element, "level", "0.5");
  const absl::Status rejected = hm::pipeline::setElementPropertyFromString(element, "level", "0.75");
  const absl::Status nan = hm::pipeline::setElementPropertyFromString(element, "level", "nan");
  const absl::Status infinity = hm::pipeline::setElementPropertyFromString(element, "float-level", "inf");
  gdouble level = 0.0;
  g_object_get(G_OBJECT(element), "level", &level, nullptr);
  const bool ok = expect(finite.ok(), finite.ToString()) &&
      expect(!rejected.ok(), "last-property-set-ok rejection must be returned to the caller") &&
      expect(!nan.ok(), "non-finite double values must be rejected") &&
      expect(!infinity.ok(), "non-finite float values must be rejected") &&
      expect(std::abs(level - 0.5) < 1e-9, "a plugin-rejected value must not appear accepted");
  gst_object_unref(element);
  return ok;
}

bool test_sensitive_values_are_redacted() {
  const std::string secret = "rtmp://a.rtmp.youtube.com/live2/abcd-efgh-secret-key";
  const std::vector<std::string> sensitive_names = {
      "location",
      "rtsp-pwd",
      "user-pw",
      "proxy-pw",
      "passphrase",
      "cookies",
      "extra-headers",
      "Authorization",
      "private-key",
      "client-key",
  };
  for (const std::string& name : sensitive_names) {
    if (!expect(hm::pipeline::isSensitivePropertyName(name), name + " should be treated as sensitive")) {
      return false;
    }
  }
  if (!expect(
          hm::pipeline::redactSensitiveValueForDisplay("location", secret) == "[redacted]",
          "location value should be redacted") ||
      !expect(
          hm::pipeline::redactSensitiveValueForDisplay("pattern", "snow") == "snow",
          "non-sensitive values should not be redacted")) {
    return false;
  }
  return true;
}

bool test_sensitive_current_and_default_values_are_never_read() {
  GstElement* element = GST_ELEMENT(g_object_new(rejecting_test_element_get_type(), nullptr));
  if (!expect(element != nullptr, "Failed to create sensitive-property test element")) {
    return false;
  }
  auto properties_or = hm::pipeline::listElementProperties(element);
  if (!expect(properties_or.ok(), properties_or.status().ToString())) {
    gst_object_unref(element);
    return false;
  }

  const std::vector<std::string> expected = {"user-pw", "extra-headers", "cookies", "client-key"};
  bool ok = true;
  for (const std::string& property_name : expected) {
    const auto property = std::find_if(properties_or->begin(), properties_or->end(), [&](const auto& candidate) {
      return candidate.name == property_name;
    });
    ok &= expect(property != properties_or->end(), property_name + " should be listed");
    if (property != properties_or->end()) {
      ok &= expect(property->secret, property_name + " must be marked sensitive");
      ok &= expect(property->serialized_value == "[redacted]", property_name + " current value must be redacted");
      ok &= expect(property->default_value == "[redacted]", property_name + " default value must be redacted");
    }
  }
  const auto* storage = reinterpret_cast<const RejectingTestElement*>(element);
  ok &= expect(storage->sensitive_get_count == 0, "sensitive getters must not run before redaction");
  gst_object_unref(element);
  return ok;
}

bool test_live_mutability_flags() {
  GParamSpec* live = g_param_spec_boolean(
      "live-prop",
      "Live Prop",
      "Mutable while playing",
      FALSE,
      static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING));
  GParamSpec* ready = g_param_spec_int(
      "ready-prop",
      "Ready Prop",
      "Mutable while ready",
      0,
      10,
      0,
      static_cast<GParamFlags>(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY));
  GParamSpec* plain = g_param_spec_string(
      "plain-prop", "Plain Prop", "No GStreamer mutability flag", "", static_cast<GParamFlags>(G_PARAM_READWRITE));

  const bool ok = expect(
                      hm::pipeline::propertyApplyMode(live) == hm::pipeline::RuntimeControlApplyMode::Live,
                      "live property apply mode should be Live") &&
      expect(hm::pipeline::propertyMutableInCurrentState(live, GST_STATE_PLAYING),
             "live property should be mutable while PLAYING") &&
      expect(hm::pipeline::propertyApplyMode(ready) == hm::pipeline::RuntimeControlApplyMode::Ready,
             "ready property apply mode should be Ready") &&
      expect(!hm::pipeline::propertyMutableInCurrentState(ready, GST_STATE_PLAYING),
             "ready property should not be mutable while PLAYING") &&
      expect(hm::pipeline::propertyMutableInCurrentState(ready, GST_STATE_READY),
             "ready property should be mutable while READY") &&
      expect(hm::pipeline::propertyApplyMode(plain) == hm::pipeline::RuntimeControlApplyMode::Restart,
             "plain writable property apply mode should be Restart");

  g_param_spec_unref(live);
  g_param_spec_unref(ready);
  g_param_spec_unref(plain);
  return ok;
}

bool test_structured_graph_and_exact_path_lookup() {
  GError* error = nullptr;
  GstElement* pipeline =
      gst_parse_launch("videotestsrc name=source num-buffers=1 ! identity name=middle ! fakesink name=sink", &error);
  if (!expect(pipeline != nullptr, error ? error->message : "Failed to build graph test pipeline")) {
    if (error) {
      g_error_free(error);
    }
    return false;
  }
  if (error) {
    g_error_free(error);
  }

  const hm::pipeline::GstPipelineGraphInfo graph = hm::pipeline::inspectPipelineGraph(pipeline);
  const auto middle = std::find_if(
      graph.elements.begin(), graph.elements.end(), [](const auto& element) { return element.name == "middle"; });
  const auto source_edge = std::find_if(graph.connections.begin(), graph.connections.end(), [](const auto& edge) {
    return edge.source_path.find("source") != std::string::npos && edge.sink_path.find("middle") != std::string::npos;
  });
  const bool graph_ok = expect(graph.elements.size() == 4, "Graph must include the pipeline and all three elements") &&
      expect(graph.connections.size() == 2, "Graph must include both linked pad connections") &&
      expect(middle != graph.elements.end() && !middle->parent_path.empty(), "Graph nodes must expose parent paths") &&
      expect(source_edge != graph.connections.end(), "Graph must expose the source-to-identity edge");

  GstElement* found =
      middle == graph.elements.end() ? nullptr : hm::pipeline::findElementByPath(pipeline, middle->path);
  GstElement* missing = hm::pipeline::findElementByPath(pipeline, "not.a.real.path");
  const bool lookup_ok = expect(found != nullptr, "Exact graph path lookup must find a listed element") &&
      expect(found && std::string(GST_ELEMENT_NAME(found)) == "middle",
             "Exact graph path lookup returned wrong node") &&
      expect(missing == nullptr, "Exact graph path lookup must reject unknown paths");
  if (found) {
    gst_object_unref(found);
  }
  gst_object_unref(pipeline);
  return graph_ok && lookup_ok;
}

bool test_length_prefixed_paths_do_not_collide() {
  GstElement* pipeline = gst_pipeline_new("collision-root");
  GstElement* dotted = gst_element_factory_make("identity", "a.b");
  GstElement* bin = gst_bin_new("a");
  GstElement* nested = gst_element_factory_make("identity", "b");
  if (!expect(pipeline && dotted && bin && nested, "Failed to create path-collision graph")) {
    if (pipeline) {
      gst_object_unref(pipeline);
    }
    return false;
  }
  gst_bin_add(GST_BIN(bin), nested);
  gst_bin_add_many(GST_BIN(pipeline), dotted, bin, nullptr);

  const hm::pipeline::GstPipelineGraphInfo graph = hm::pipeline::inspectPipelineGraph(pipeline);
  const auto dotted_info = std::find_if(
      graph.elements.begin(), graph.elements.end(), [](const auto& element) { return element.name == "a.b"; });
  const auto nested_info = std::find_if(graph.elements.begin(), graph.elements.end(), [](const auto& element) {
    return element.name == "b" && element.parent_path.find("1:a") != std::string::npos;
  });
  bool ok = expect(dotted_info != graph.elements.end(), "Dotted sibling must be present") &&
      expect(nested_info != graph.elements.end(), "Nested element must be present") &&
      expect(dotted_info != graph.elements.end() && nested_info != graph.elements.end() &&
                 dotted_info->path != nested_info->path,
             "Length-prefixed component paths must distinguish sibling a.b from nested a/b");

  GstElement* dotted_target =
      dotted_info == graph.elements.end() ? nullptr : hm::pipeline::findElementByPath(pipeline, dotted_info->path);
  GstElement* nested_target =
      nested_info == graph.elements.end() ? nullptr : hm::pipeline::findElementByPath(pipeline, nested_info->path);
  if (dotted_target && nested_target) {
    const absl::Status status = hm::pipeline::setElementPropertyFromString(dotted_target, "silent", "false");
    gboolean dotted_silent = TRUE;
    gboolean nested_silent = TRUE;
    g_object_get(G_OBJECT(dotted_target), "silent", &dotted_silent, nullptr);
    g_object_get(G_OBJECT(nested_target), "silent", &nested_silent, nullptr);
    ok &= expect(status.ok(), status.ToString()) &&
        expect(!dotted_silent && nested_silent, "Exact encoded lookup/edit must target only the dotted sibling");
  } else {
    ok = false;
  }
  if (dotted_target) {
    gst_object_unref(dotted_target);
  }
  if (nested_target) {
    gst_object_unref(nested_target);
  }
  gst_object_unref(pipeline);
  return ok;
}

bool test_negotiated_caps_are_not_exposed() {
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "videotestsrc num-buffers=30 ! capsfilter name=sensitive_caps "
      "caps=\"video/x-raw,width=16,height=16,uri=(string)secret-uri,"
      "authorization=(string)Bearer-secret,client-key=(string)private-key,codec_data=(buffer)00112233\" "
      "! fakesink sync=false",
      &error);
  if (!expect(pipeline != nullptr, error ? error->message : "Failed to create sensitive-caps graph")) {
    if (error) {
      g_error_free(error);
    }
    return false;
  }
  if (error) {
    g_error_free(error);
  }
  gst_element_set_state(pipeline, GST_STATE_PAUSED);
  gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND * 2);
  GstElement* filter = gst_bin_get_by_name(GST_BIN(pipeline), "sensitive_caps");
  GstPad* pad = filter ? gst_element_get_static_pad(filter, "src") : nullptr;
  GstCaps* caps = pad ? gst_pad_get_current_caps(pad) : nullptr;
  gchar* serialized_caps = caps ? gst_caps_to_string(caps) : nullptr;
  const std::string negotiated = serialized_caps ? serialized_caps : "";
  bool ok = expect(
      negotiated.find("secret-uri") != std::string::npos && negotiated.find("Bearer-secret") != std::string::npos &&
          negotiated.find("private-key") != std::string::npos && negotiated.find("codec_data") != std::string::npos,
      "Sensitive URI/header/key/buffer-like fields must be present in the negotiated test caps");

  const hm::pipeline::GstPipelineGraphInfo graph = hm::pipeline::inspectPipelineGraph(pipeline);
  std::string exposed;
  for (const auto& element : graph.elements) {
    exposed += element.path + element.parent_path + element.name + element.type_name + element.factory_name;
  }
  for (const auto& edge : graph.connections) {
    exposed += edge.source_path + edge.source_pad + edge.sink_path + edge.sink_pad;
  }
  ok &= expect(
      exposed.find("secret-uri") == std::string::npos && exposed.find("Bearer-secret") == std::string::npos &&
          exposed.find("private-key") == std::string::npos && exposed.find("00112233") == std::string::npos,
      "Negotiated caps contents must never enter inspector graph metadata");

  g_free(serialized_caps);
  if (caps) {
    gst_caps_unref(caps);
  }
  if (pad) {
    gst_object_unref(pad);
  }
  if (filter) {
    gst_object_unref(filter);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!test_list_and_set_identity_properties() || !test_enum_metadata_and_parsing() ||
      !test_invalid_values_are_rejected() || !test_non_finite_and_plugin_rejected_values_are_rejected() ||
      !test_sensitive_values_are_redacted() || !test_sensitive_current_and_default_values_are_never_read() ||
      !test_live_mutability_flags() || !test_structured_graph_and_exact_path_lookup() ||
      !test_length_prefixed_paths_do_not_collide() || !test_negotiated_caps_are_not_exposed()) {
    return 1;
  }
  return 0;
}
