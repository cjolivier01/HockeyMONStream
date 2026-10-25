#pragma once

#include <gst/gst.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hm::preview {

struct RouteTap {
  enum class Kind { kExistingTee, kAfterElement, kReplaceDrain };
  Kind kind;
  GstElement* parent;
  GstElement* upstream;
  // kReplaceDrain preserves this queue and its existing tee request pad.
  GstElement* queue{nullptr};
  GstElement* replaced_sink{nullptr};
  // Only kAfterElement creates a tee. Program always uses kExistingTee.
  std::string tee_name;
};

// Transactional construction at a stopped native graph tap. No window IDs,
// rendering, encoding, defaults or frame memory live here. The owner serializes
// graph mutations and keeps the tap's parent at NULL/READY during attachment.
//
// Create consumes every unparented element in path, including on failure. The
// one exception is tap.queue, borrowed at its existing position in path. The
// chain must start with ingress and retain queue -> drain ordering. A terminal
// consumer may be in path (Qt), or external_sink may name a shared target's
// input (browser); explicit ghost pads bridge only the necessary parent bins.
//
// Until Commit succeeds, destruction removes new elements/request/ghost pads
// and restores the original tracked/source links and source queue settings.
// Committed nodes/pads belong to their graph parents. Retire callbacks before
// destroying/replacing those parents, as for ordinary native graph branches.
class RouteAttachment {
 public:
  static std::unique_ptr<RouteAttachment> Create(
      RouteTap tap,
      std::vector<GstElement*> path,
      GstPad* external_sink,
      std::string* error);
  ~RouteAttachment();
  RouteAttachment(const RouteAttachment&) = delete;
  RouteAttachment& operator=(const RouteAttachment&) = delete;
  // Publish borrowed route callbacks only after all links/configuration exist.
  // publish must leave no state behind on false. It cannot throw.
  bool Commit(const std::function<bool()>& publish);

 private:
  struct State;
  explicit RouteAttachment(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

} // namespace hm::preview
