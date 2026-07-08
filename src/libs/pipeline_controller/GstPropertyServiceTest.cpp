#include "hstream/src/libs/pipeline_controller/GstPropertyService.h"

#include <gst/gst.h>

#include <iostream>
#include <string>

namespace {

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

bool test_sensitive_values_are_redacted() {
  const std::string secret = "rtmp://a.rtmp.youtube.com/live2/abcd-efgh-secret-key";
  if (!expect(hm::pipeline::isSensitivePropertyName("location"), "location should be treated as sensitive") ||
      !expect(hm::pipeline::isSensitivePropertyName("rtsp-pwd"), "rtsp-pwd should be treated as sensitive") ||
      !expect(
          hm::pipeline::redactSensitiveValueForDisplay("location", secret) == "[redacted]",
          "location value should be redacted") ||
      !expect(
          hm::pipeline::redactSensitiveValueForDisplay("pattern", "snow") == "snow",
          "non-sensitive values should not be redacted")) {
    return false;
  }
  return true;
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

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!test_list_and_set_identity_properties() || !test_enum_metadata_and_parsing() ||
      !test_invalid_values_are_rejected() || !test_sensitive_values_are_redacted() || !test_live_mutability_flags()) {
    return 1;
  }
  return 0;
}
