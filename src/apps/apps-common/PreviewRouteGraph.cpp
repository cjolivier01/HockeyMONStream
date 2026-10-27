#include "PreviewRouteGraph.h"

#include <algorithm>
#include <set>
#include <utility>

namespace hm::preview {

bool AttachPlaybackClock(RouteTap program, std::string* error) {
  auto* queue = gst_element_factory_make("queue", "playback_clock_queue");
  auto* sink = gst_element_factory_make("fakesink", "playback_clock");
  if (queue)
    g_object_set(queue, "max-size-buffers", 1U, "max-size-bytes", 0U, "max-size-time", guint64{0}, "leaky", 0, nullptr);
  if (sink)
    g_object_set(
        sink,
        "sync",
        TRUE,
        "async",
        TRUE,
        "max-lateness",
        gint64{-1},
        "qos",
        FALSE,
        "enable-last-sample",
        FALSE,
        nullptr);
  auto attachment = RouteAttachment::Create(program, {queue, sink}, nullptr, error);
  return attachment && attachment->Commit([] { return true; });
}
namespace {
struct PadPair {
  GstPad *source, *sink;
};
struct RequestPad {
  GstElement* tee;
  GstPad* pad;
};
struct GhostPad {
  GstElement* parent;
  GstPad* pad;
};
GstPad* pad(GstElement* element, const char* name) {
  return element ? gst_element_get_static_pad(element, name) : nullptr;
}
void refresh_sink_ancestors(GstElement* element) {
  // GstBin updates its own SINK flag when its last sink is removed, but does
  // not propagate that change through bins which were already parented.
  // A source route moved to a shared external target otherwise leaves a
  // phantom sink in pipeline EOS aggregation. Attachments run while stopped.
  for (auto* current = GST_OBJECT(gst_object_ref(element)); current;) {
    if (GST_IS_BIN(current)) {
      auto* children = gst_bin_iterate_elements(GST_BIN(current));
      GValue value = G_VALUE_INIT;
      bool has_sink = false;
      while (gst_iterator_next(children, &value) == GST_ITERATOR_OK) {
        has_sink |= GST_OBJECT_FLAG_IS_SET(g_value_get_object(&value), GST_ELEMENT_FLAG_SINK);
        g_value_reset(&value);
      }
      if (G_VALUE_TYPE(&value))
        g_value_unset(&value);
      gst_iterator_free(children);
      GST_OBJECT_LOCK(current);
      if (has_sink)
        GST_OBJECT_FLAG_SET(current, GST_ELEMENT_FLAG_SINK);
      else
        GST_OBJECT_FLAG_UNSET(current, GST_ELEMENT_FLAG_SINK);
      GST_OBJECT_UNLOCK(current);
    }
    auto* parent = gst_object_get_parent(current);
    gst_object_unref(current);
    current = parent;
  }
}
} // namespace

struct RouteAttachment::State {
  RouteTap tap;
  std::vector<GstElement*> path, owned, added;
  std::vector<PadPair> links, originals;
  std::vector<RequestPad> requests;
  std::vector<GhostPad> ghosts;
  GstElement* tee{nullptr};
  GstElement* old_sink{nullptr};
  bool committed{false}, sink_removed{false}, have_queue_settings{false};
  gint leaky{0};
  guint max_buffers{0}, max_bytes{0};
  guint64 max_time{0};
  std::string* error;

  explicit State(RouteTap tap, std::vector<GstElement*> path, std::string* error)
      : tap(std::move(tap)), path(std::move(path)), error(error) {
    // Own each new node exactly once even if malformed input repeats a node.
    std::set<GstElement*> unique;
    for (auto* element : this->path) {
      if (element && element != this->tap.queue && !GST_OBJECT_PARENT(element) && unique.insert(element).second) {
        gst_object_ref_sink(element);
        owned.push_back(element);
      }
    }
  }
  bool Fail(const char* message) {
    if (error)
      *error = message;
    return false;
  }
  bool Add(GstElement* element) {
    if (!gst_bin_add(GST_BIN(tap.parent), element))
      return Fail("Could not parent a preview path element");
    added.push_back(element);
    return true;
  }
  bool Link(GstPad* source, GstPad* sink) {
    if (!source || !sink || gst_pad_link(source, sink) != GST_PAD_LINK_OK)
      return Fail("Could not link a preview path pad");
    links.push_back({GST_PAD(gst_object_ref(source)), GST_PAD(gst_object_ref(sink))});
    return true;
  }
  bool Link(GstElement* source, GstElement* sink) {
    auto* out = pad(source, "src");
    auto* in = pad(sink, "sink");
    const bool result = Link(out, in);
    if (out)
      gst_object_unref(out);
    if (in)
      gst_object_unref(in);
    return result;
  }
  bool Detach(GstPad* source, GstPad* sink) {
    if (!source || !sink || !gst_pad_unlink(source, sink))
      return Fail("Could not detach the original preview tap");
    originals.push_back({GST_PAD(gst_object_ref(source)), GST_PAD(gst_object_ref(sink))});
    return true;
  }
  GstPad* Request(GstElement* element) {
    auto* type = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(element), "src_%u");
    auto* result = type ? gst_element_request_pad(element, type, nullptr, nullptr) : nullptr;
    if (result)
      requests.push_back({element, result});
    else
      Fail("Could not request a preview tee pad");
    return result;
  }
  bool Bridge(GstPad* source, GstPad* sink) {
    if (!source || !sink || GST_PAD_DIRECTION(sink) != GST_PAD_SINK)
      return Fail("Shared preview consumer has no sink pad");
    auto* source_owner = gst_pad_get_parent_element(source);
    auto* sink_owner = gst_pad_get_parent_element(sink);
    if (!source_owner || !sink_owner) {
      if (source_owner)
        gst_object_unref(source_owner);
      if (sink_owner)
        gst_object_unref(sink_owner);
      return Fail("Shared preview pads must have graph owners");
    }
    std::vector<GstObject*> ancestors;
    for (GstObject* current = gst_object_get_parent(GST_OBJECT(source_owner)); current;) {
      ancestors.push_back(current);
      current = gst_object_get_parent(current);
    }
    GstObject* common = gst_object_get_parent(GST_OBJECT(sink_owner));
    while (common && std::find(ancestors.begin(), ancestors.end(), common) == ancestors.end()) {
      auto* parent = gst_object_get_parent(common);
      gst_object_unref(common);
      common = parent;
    }
    auto lift = [&](GstElement* owner, GstPad* input) -> GstPad* {
      GstPad* current = GST_PAD(gst_object_ref(input));
      auto* parent = gst_object_get_parent(GST_OBJECT(owner));
      while (parent && parent != common) {
        auto* ghost = GST_IS_BIN(parent) ? gst_ghost_pad_new(nullptr, current) : nullptr;
        if (!ghost || !gst_element_add_pad(GST_ELEMENT(parent), ghost)) {
          if (ghost)
            gst_object_unref(ghost);
          gst_object_unref(current);
          gst_object_unref(parent);
          return nullptr;
        }
        ghosts.push_back({GST_ELEMENT(parent), GST_PAD(gst_object_ref(ghost))});
        gst_object_unref(current);
        current = GST_PAD(gst_object_ref(ghost));
        // Newly built source bins can be nested in a retained paused graph.
        if (GST_STATE(parent) >= GST_STATE_PAUSED)
          gst_pad_set_active(ghost, TRUE);
        parent = gst_object_get_parent(parent);
      }
      if (parent)
        gst_object_unref(parent);
      return current;
    };
    GstPad* out = common ? lift(source_owner, source) : nullptr;
    GstPad* in = common ? lift(sink_owner, sink) : nullptr;
    const bool result = out && in && Link(out, in);
    if (!result)
      Fail("Could not bridge preview pads within one graph");
    if (out)
      gst_object_unref(out);
    if (in)
      gst_object_unref(in);
    if (common)
      gst_object_unref(common);
    for (auto* ancestor : ancestors)
      gst_object_unref(ancestor);
    gst_object_unref(source_owner);
    gst_object_unref(sink_owner);
    return result;
  }
  bool Build(GstPad* external) {
    if (!tap.parent || !GST_IS_BIN(tap.parent) || !tap.upstream ||
        GST_OBJECT_PARENT(tap.upstream) != GST_OBJECT(tap.parent) || path.empty())
      return Fail("Preview tap requires a parent and an upstream element in that parent");
    if (GST_STATE(tap.parent) > GST_STATE_READY || GST_STATE_PENDING(tap.parent) > GST_STATE_READY)
      return Fail("Preview attachment requires its parent to be stopped");
    std::set<GstElement*> unique;
    for (auto* element : path) {
      if (!element || !unique.insert(element).second ||
          (element == tap.queue ? GST_OBJECT_PARENT(element) != GST_OBJECT(tap.parent)
                                : GST_OBJECT_PARENT(element) != nullptr))
        return Fail("Preview path contains a missing, repeated or already parented element");
    }
    if (tap.kind == RouteTap::Kind::kReplaceDrain) {
      if (!tap.queue || !tap.replaced_sink || GST_OBJECT_PARENT(tap.replaced_sink) != GST_OBJECT(tap.parent) ||
          path.size() < 3 || path[1] != tap.queue)
        return Fail("Source preview must preserve its queue after ingress");
      old_sink = GST_ELEMENT(gst_object_ref(tap.replaced_sink));
      g_object_get(
          tap.queue,
          "leaky",
          &leaky,
          "max-size-buffers",
          &max_buffers,
          "max-size-bytes",
          &max_bytes,
          "max-size-time",
          &max_time,
          nullptr);
      have_queue_settings = true;
    }
    for (auto* element : path)
      if (element != tap.queue && !Add(element))
        return false;
    GstPad *original_source = nullptr, *original_sink = nullptr;
    if (tap.kind == RouteTap::Kind::kAfterElement) {
      if (tap.tee_name.empty())
        return Fail("Tracked preview requires an explicit tee name");
      original_source = pad(tap.upstream, "src");
      original_sink = original_source ? gst_pad_get_peer(original_source) : nullptr;
      tee = gst_element_factory_make("tee", tap.tee_name.c_str());
      if (tee) {
        gst_object_ref_sink(tee);
        owned.push_back(tee);
      }
      if (!tee || !Add(tee)) {
        if (original_source)
          gst_object_unref(original_source);
        if (original_sink)
          gst_object_unref(original_sink);
        return Fail("Could not create the tracked preview tee");
      }
    } else if (tap.kind == RouteTap::Kind::kReplaceDrain) {
      original_sink = pad(tap.queue, "sink");
      original_source = original_sink ? gst_pad_get_peer(original_sink) : nullptr;
      if (original_source && GST_OBJECT_PARENT(original_source) != GST_OBJECT(tap.upstream)) {
        gst_object_unref(original_source);
        original_source = nullptr;
      }
    }
    bool detached = true;
    if (tap.kind != RouteTap::Kind::kExistingTee)
      detached = Detach(original_source, original_sink);
    if (detached && tap.kind == RouteTap::Kind::kReplaceDrain) {
      auto* out = pad(tap.queue, "src");
      auto* in = pad(old_sink, "sink");
      auto* peer = out ? gst_pad_get_peer(out) : nullptr;
      detached = peer == in && Detach(out, in);
      if (peer)
        gst_object_unref(peer);
      if (out)
        gst_object_unref(out);
      if (in)
        gst_object_unref(in);
    }
    if (detached)
      for (std::size_t index = 1; index < path.size(); ++index)
        if (!(detached = Link(path[index - 1], path[index])))
          break;
    if (detached && external) {
      auto* out = pad(path.back(), "src");
      detached = Bridge(out, external);
      if (out)
        gst_object_unref(out);
    }
    if (detached) {
      auto* first = pad(path.front(), "sink");
      if (tap.kind == RouteTap::Kind::kAfterElement)
        detached = Link(tap.upstream, tee) && Link(Request(tee), original_sink) && Link(Request(tee), first);
      else if (tap.kind == RouteTap::Kind::kReplaceDrain)
        detached = Link(original_source, first);
      else
        detached = Link(Request(tap.upstream), first);
      if (first)
        gst_object_unref(first);
    }
    if (original_source)
      gst_object_unref(original_source);
    if (original_sink)
      gst_object_unref(original_sink);
    return detached;
  }
  ~State() {
    if (!committed) {
      for (auto it = links.rbegin(); it != links.rend(); ++it)
        gst_pad_unlink(it->source, it->sink);
      for (auto it = ghosts.rbegin(); it != ghosts.rend(); ++it) {
        gst_ghost_pad_set_target(GST_GHOST_PAD(it->pad), nullptr);
        gst_element_remove_pad(it->parent, it->pad);
      }
      if (sink_removed)
        gst_bin_add(GST_BIN(tap.parent), old_sink);
      for (const auto& original : originals)
        if (gst_pad_link(original.source, original.sink) != GST_PAD_LINK_OK)
          g_critical("Could not restore a native preview tap after failed attachment");
      if (have_queue_settings)
        g_object_set(
            tap.queue,
            "leaky",
            leaky,
            "max-size-buffers",
            max_buffers,
            "max-size-bytes",
            max_bytes,
            "max-size-time",
            max_time,
            nullptr);
      for (const auto& request : requests)
        gst_element_release_request_pad(request.tee, request.pad);
      for (auto it = added.rbegin(); it != added.rend(); ++it)
        gst_bin_remove(GST_BIN(tap.parent), *it);
    }
    for (const auto& link : links) {
      gst_object_unref(link.source);
      gst_object_unref(link.sink);
    }
    for (const auto& link : originals) {
      gst_object_unref(link.source);
      gst_object_unref(link.sink);
    }
    for (const auto& request : requests)
      gst_object_unref(request.pad);
    for (const auto& ghost : ghosts) {
      gst_object_unref(ghost.pad);
      gst_object_unref(ghost.parent);
    }
    for (auto* element : owned)
      gst_object_unref(element);
    if (old_sink)
      gst_object_unref(old_sink);
  }
};

RouteAttachment::RouteAttachment(std::unique_ptr<State> state) : state_(std::move(state)) {}
RouteAttachment::~RouteAttachment() = default;
std::unique_ptr<RouteAttachment> RouteAttachment::Create(
    RouteTap tap,
    std::vector<GstElement*> path,
    GstPad* external_sink,
    std::string* error) {
  if (error)
    error->clear();
  auto state = std::make_unique<State>(std::move(tap), std::move(path), error);
  if (!state->Build(external_sink))
    return {};
  return std::unique_ptr<RouteAttachment>(new RouteAttachment(std::move(state)));
}
bool RouteAttachment::Commit(const std::function<bool()>& publish) {
  if (state_->committed)
    return false;
  if (state_->old_sink) {
    if (!gst_bin_remove(GST_BIN(state_->tap.parent), state_->old_sink))
      return false;
    state_->sink_removed = true;
  }
  if (publish && !publish())
    return false;
  state_->committed = true;
  if (state_->sink_removed)
    refresh_sink_ancestors(state_->tap.parent);
  return true;
}
} // namespace hm::preview
