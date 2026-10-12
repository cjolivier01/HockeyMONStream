#include "hstream/src/apps/apps-common/UriPlaylistCapturePoolDrain.h"

#include <gst/gst.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <thread>

struct PoolLifecycle {
  bool stopped = false;
  bool finalized = false;
  bool weak_notified = false;
  bool wedge = false;
};
struct FinalizingPool {
  GstBufferPool parent;
  PoolLifecycle* lifecycle;
};
struct FinalizingPoolClass {
  GstBufferPoolClass parent;
};
G_DEFINE_TYPE(FinalizingPool, finalizing_pool, GST_TYPE_BUFFER_POOL)

static gboolean finalizing_pool_stop(GstBufferPool* pool) {
  const gboolean stopped = GST_BUFFER_POOL_CLASS(finalizing_pool_parent_class)->stop(pool);
  reinterpret_cast<FinalizingPool*>(pool)->lifecycle->stopped = stopped;
  return stopped;
}
static void finalizing_pool_finalize(GObject* object) {
  auto* lifecycle = reinterpret_cast<FinalizingPool*>(object)->lifecycle;
  while (lifecycle->wedge) {
    g_usleep(10000);
  }
  G_OBJECT_CLASS(finalizing_pool_parent_class)->finalize(object);
  lifecycle->finalized = true;
}
static void finalizing_pool_class_init(FinalizingPoolClass* klass) {
  GST_BUFFER_POOL_CLASS(klass)->stop = finalizing_pool_stop;
  G_OBJECT_CLASS(klass)->finalize = finalizing_pool_finalize;
}
static void finalizing_pool_init(FinalizingPool*) {}

static GstBufferPool* make_pool(PoolLifecycle* lifecycle) {
  auto* pool = GST_BUFFER_POOL(g_object_new(finalizing_pool_get_type(), nullptr));
  reinterpret_cast<FinalizingPool*>(pool)->lifecycle = lifecycle;
  GstStructure* config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(config, nullptr, 64, 0, 2);
  if (!gst_buffer_pool_set_config(pool, config) || !gst_buffer_pool_set_active(pool, TRUE)) {
    gst_object_unref(pool);
    return nullptr;
  }
  g_object_weak_ref(
      G_OBJECT(pool),
      [](gpointer data, GObject*) {
        auto* lifecycle = static_cast<PoolLifecycle*>(data);
        lifecycle->weak_notified = !lifecycle->finalized;
      },
      lifecycle);
  return pool;
}

// Preserve the real GStreamer ownership chain: a writable shallow copy keeps
// the decoder's pooled buffer alive through ParentBufferMeta. Metadata-only
// output copying must not make the input's resources appear drained early.
static bool borrowed_frame_delays_completed_destruction() {
  PoolLifecycle lifecycle;
  GstBufferPool* pool = make_pool(&lifecycle);
  if (!pool)
    return false;
  GstBuffer* frame = nullptr;
  if (gst_buffer_pool_acquire_buffer(pool, &frame, nullptr) != GST_FLOW_OK)
    return false;
  hm::UriPlaylistCapturePoolDrain drain;
  drain.Observe(frame);
  drain.Observe(frame);
  GstBuffer* borrowed = gst_buffer_copy(frame);
  GstBuffer* owned_output = gst_buffer_new();
  gst_buffer_copy_into(owned_output, frame, GST_BUFFER_COPY_METADATA, 0, -1);
  const bool has_parent = gst_buffer_get_parent_buffer_meta(borrowed) != nullptr;
  gst_buffer_unref(frame);
  gst_buffer_pool_set_active(pool, FALSE);
  gst_object_unref(pool); // The decoder owner leaves; borrowed input remains.
  const bool waits_for_input = !drain.Drain() && !lifecycle.finalized;
  gst_buffer_unref(borrowed);
  const bool keeps_final_unref = lifecycle.stopped && !lifecycle.finalized;
  const bool completes = drain.Drain() && lifecycle.finalized && lifecycle.weak_notified;
  gst_buffer_unref(owned_output);
  return has_parent && waits_for_input && keeps_final_unref && completes;
}

static bool distinct_pools_and_teardown() {
  PoolLifecycle first, second;
  GstBufferPool* pools[] = {make_pool(&first), make_pool(&second)};
  if (!pools[0] || !pools[1])
    return false;
  GstBuffer* frames[2]{};
  {
    hm::UriPlaylistCapturePoolDrain drain;
    for (unsigned i = 0; i < 2; ++i) {
      if (gst_buffer_pool_acquire_buffer(pools[i], &frames[i], nullptr) != GST_FLOW_OK)
        return false;
      drain.Observe(frames[i]);
      gst_buffer_pool_set_active(pools[i], FALSE);
      gst_object_unref(pools[i]);
    }
    gst_buffer_unref(frames[0]);
    if (drain.Drain() || !first.finalized || second.finalized)
      return false;
    // Cancellation releases the retained owner without waiting on a borrower.
  }
  if (second.finalized)
    return false;
  gst_buffer_unref(frames[1]);
  return second.finalized;
}

static bool blocking_finalizer_is_bounded() {
  const pid_t child = fork();
  if (child == 0) {
    PoolLifecycle lifecycle;
    GstBufferPool* pool = make_pool(&lifecycle);
    GstBuffer* frame = nullptr;
    if (!pool || gst_buffer_pool_acquire_buffer(pool, &frame, nullptr) != GST_FLOW_OK)
      _exit(1);
    hm::UriPlaylistCapturePoolDrain drain;
    drain.Observe(frame);
    gst_buffer_unref(frame);
    gst_buffer_pool_set_active(pool, FALSE);
    gst_object_unref(pool);
    lifecycle.wedge = true;
    drain.Drain(G_TIME_SPAN_SECOND);
    _exit(1);
  }
  if (child < 0)
    return false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(child, &status, WNOHANG) == child)
      return WIFEXITED(status) && WEXITSTATUS(status) == 70;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  return false;
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!borrowed_frame_delays_completed_destruction() || !distinct_pools_and_teardown() ||
      !blocking_finalizer_is_bounded()) {
    std::cerr << "Capture-pool lifetime fence failed\n";
    return 1;
  }
  return 0;
}
