#include "WebPreviewObserver.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hm::preview {
namespace {
const char* string(JsonObject* object, const char* member) {
  auto* node = json_object_get_member(object, member);
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
      ? json_node_get_string(node)
      : nullptr;
}
bool integer(JsonObject* object, const char* member, std::int64_t* result) {
  auto* node = json_object_get_member(object, member);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
    return false;
  *result = json_node_get_int(node);
  return true;
}
bool decimal(JsonObject* object, const char* member, std::uint64_t* result) {
  const char* value = string(object, member);
  if (!value || !*value || *value == '0')
    return false;
  const auto size = strlen(value);
  if (size > 20)
    return false;
  const auto parsed = std::from_chars(value, value + size, *result);
  return parsed.ec == std::errc{} && parsed.ptr == value + size;
}
void text(JsonBuilder* b, const char* key, const std::string& value) {
  json_builder_set_member_name(b, key);
  json_builder_add_string_value(b, value.c_str());
}
void number(JsonBuilder* b, const char* key, std::int64_t value) {
  json_builder_set_member_name(b, key);
  json_builder_add_int_value(b, value);
}
void sequence(JsonBuilder* b, const char* key, std::uint64_t value) {
  text(b, key, std::to_string(value));
}
} // namespace

WebPreviewObserver::WebPreviewObserver(std::shared_ptr<TransportMonitor> monitor, WebPreviewObserverConfig config)
    : monitor_(std::move(monitor)), config_(std::move(config)) {
  if (!monitor_ || config_.session.run.empty() || config_.session.run.size() > 128 || !config_.session.topology ||
      !config_.poll_interval_ms || config_.poll_interval_ms > 60000 || !config_.events_per_batch ||
      config_.events_per_batch > 256 || !g_utf8_validate(config_.session.run.c_str(), -1, nullptr) ||
      config_.session.run.find('\0') != std::string::npos)
    throw std::invalid_argument("Preview observer requires a bounded session, polling interval and event budget");
}

bool WebPreviewObserver::Retire(std::uint64_t peer) {
  if (!peer || peer <= peer_)
    return false;
  peer_ = peer;
  active_ = false;
  outstanding_batch_ = 0;
  monitor_->Recover();
  return true;
}

bool WebPreviewObserver::Activate(std::uint64_t peer) {
  if (!peer || peer != peer_ || active_)
    return false;
  active_ = true;
  return true;
}

std::optional<std::string> WebPreviewObserver::Poll() {
  auto events = monitor_->TakeEvents();
  if (!active_ || outstanding_batch_ || next_batch_ == std::numeric_limits<std::uint64_t>::max()) {
    dropped_events_ += events.size();
    return {};
  }
  events.erase(
      std::remove_if(events.begin(), events.end(), [&](const auto& event) { return event.frame.peer != peer_; }),
      events.end());
  if (events.size() > config_.events_per_batch) {
    dropped_events_ += events.size() - config_.events_per_batch;
    events.erase(events.begin(), events.end() - config_.events_per_batch);
  }
  if (events.empty())
    return {};
  auto* builder = json_builder_new();
  json_builder_begin_object(builder);
  text(builder, "type", "preview-frames");
  number(builder, "version", 1);
  text(builder, "run", config_.session.run);
  number(builder, "stage", config_.session.stage);
  sequence(builder, "topology", config_.session.topology);
  sequence(builder, "peer", peer_);
  outstanding_batch_ = ++next_batch_;
  sequence(builder, "batch", outstanding_batch_);
  json_builder_set_member_name(builder, "events");
  json_builder_begin_array(builder);
  for (const auto& event : events) {
    json_builder_begin_object(builder);
    text(builder, "boundary", event.kind == TransportEvent::Kind::kEncoded ? "encoded" : "packetized");
    text(builder, "channel", event.frame.channel);
    sequence(builder, "operation", event.frame.operation);
    sequence(builder, "sequence", event.frame.sequence);
    sequence(builder, "sourcePts", event.frame.source_pts);
    sequence(builder, "presentationPts", event.frame.presentation_pts);
    if (event.kind == TransportEvent::Kind::kPacketized) {
      number(builder, "rtpTimestamp", event.rtp_timestamp);
      number(builder, "ssrc", event.ssrc);
    }
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  auto* root = json_builder_get_root(builder);
  auto* generator = json_generator_new();
  json_generator_set_root(generator, root);
  auto* serialized = json_generator_to_data(generator, nullptr);
  std::string result(serialized);
  g_free(serialized);
  g_object_unref(generator);
  json_node_free(root);
  g_object_unref(builder);
  return result;
}

bool WebPreviewObserver::Matches(JsonObject* object) const {
  const auto* run = string(object, "run");
  std::int64_t version, stage;
  std::uint64_t topology, peer;
  return active_ && run && config_.session.run == run && integer(object, "version", &version) && version == 1 &&
      integer(object, "stage", &stage) && stage == config_.session.stage && decimal(object, "topology", &topology) &&
      topology == config_.session.topology && decimal(object, "peer", &peer) && peer == peer_;
}

WebPreviewObserver::Message WebPreviewObserver::Handle(JsonObject* object) {
  const auto rejected = [&] {
    ++rejected_messages_;
    return Message::kRejected;
  };
  const auto* type = object ? string(object, "type") : nullptr;
  if (!type || (strcmp(type, "preview-frames-ack") && strcmp(type, "preview-presented")))
    return Message::kUnrecognized;
  if (!Matches(object))
    return rejected();
  if (!strcmp(type, "preview-frames-ack")) {
    std::uint64_t batch;
    if (json_object_get_size(object) != 7 || !decimal(object, "batch", &batch) || batch != outstanding_batch_)
      return rejected();
    outstanding_batch_ = 0;
    return Message::kAcknowledged;
  }
  PacketFrame receipt{};
  receipt.peer = peer_;
  const auto* channel = string(object, "channel");
  const auto* observation = string(object, "observation");
  std::int64_t timestamp, ssrc;
  if (json_object_get_size(object) != 12 || !channel || !*channel || strlen(channel) > 64 || !observation ||
      strcmp(observation, "video-frame-callback") || !decimal(object, "operation", &receipt.operation) ||
      !decimal(object, "sequence", &receipt.sequence) || !integer(object, "rtpTimestamp", &timestamp) ||
      timestamp < 0 || timestamp > UINT32_MAX || !integer(object, "ssrc", &ssrc) || ssrc < 0 || ssrc > UINT32_MAX)
    return rejected();
  receipt.channel = channel;
  receipt.rtp_timestamp = timestamp;
  receipt.ssrc = ssrc;
  auto accepted = monitor_->Presented(receipt);
  if (!accepted)
    return rejected();
  return Message::kPresented;
}

std::optional<PacketFrame> WebPreviewObserver::TakePresented() {
  return monitor_->TakePresented();
}
} // namespace hm::preview
