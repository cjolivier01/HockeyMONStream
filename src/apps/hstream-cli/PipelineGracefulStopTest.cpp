#include "hstream/src/apps/apps-common/UriPlaylistCapturePoolDrain.h"
#include "src/apps/hstream-cli/deepstream_app.h"

#include <gst/gst.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <thread>

GST_DEBUG_CATEGORY(NVDS_APP);

// Model a decoder stuck in source retirement, before the graph NULL transition.
struct StuckSource {
  GstBin parent;
};
struct StuckSourceClass {
  GstBinClass parent;
};
G_DEFINE_TYPE(StuckSource, stuck_source, GST_TYPE_BIN)

static GstStateChangeReturn stuck_source_change_state(GstElement* element, GstStateChange transition) {
  if (transition == GST_STATE_CHANGE_NULL_TO_READY && g_object_get_data(G_OBJECT(element), "retired")) {
    g_object_set_data(G_OBJECT(element), "restarted", GINT_TO_POINTER(1));
  }
  if (transition == GST_STATE_CHANGE_READY_TO_NULL) {
    g_object_set_data(G_OBJECT(element), "retired", GINT_TO_POINTER(1));
  }
  if (transition == GST_STATE_CHANGE_READY_TO_NULL && g_object_get_data(G_OBJECT(element), "wedge")) {
    for (;;) {
      g_usleep(10000);
    }
  }
  if (transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING && g_object_get_data(G_OBJECT(element), "async-play")) {
    return GST_STATE_CHANGE_ASYNC;
  }
  return GST_ELEMENT_CLASS(stuck_source_parent_class)->change_state(element, transition);
}
static void stuck_source_class_init(StuckSourceClass* klass) {
  GST_ELEMENT_CLASS(klass)->change_state = stuck_source_change_state;
  G_OBJECT_CLASS(klass)->set_property = [](GObject* object, guint, const GValue* value, GParamSpec*) {
    g_object_set_data_full(object, "test-uri", g_value_dup_string(value), g_free);
  };
  G_OBJECT_CLASS(klass)->get_property = [](GObject* object, guint, GValue* value, GParamSpec*) {
    g_value_set_string(value, static_cast<const char*>(g_object_get_data(object, "test-uri")));
  };
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), 1, g_param_spec_string("uri", "URI", "Chapter URI", nullptr, G_PARAM_READWRITE));
}
static void stuck_source_init(StuckSource*) {}

namespace {

int run_stop(bool wedged, const char* watchdog_seconds) {
  g_setenv("HM_SHUTDOWN_WATCHDOG_SECONDS", watchdog_seconds, TRUE);
  auto owned_app = std::make_unique<AppCtx>();
  AppCtx& app = *owned_app;
  app.pipeline.pipeline = gst_pipeline_new(nullptr);
  app.eos_received = TRUE;
  auto& sources = app.pipeline.multi_src_bin;
  g_mutex_init(&sources.uri_playlist_barrier_mutex);
  g_cond_init(&sources.uri_playlist_barrier_cond);
  sources.uri_playlist_barrier_initialized = TRUE;
  sources.num_bins = 1;
  auto& source = sources.sub_bins[0];
  source.parent_bin = &sources;
  source.num_uri_list = 1;
  source.uri_list = g_new0(gchar*, 2);
  source.uri_list[0] = g_strdup("file:///graceful-stop-test.mp4");
  source.src_elem = GST_ELEMENT(g_object_new(stuck_source_get_type(), nullptr));
  if (wedged) {
    g_object_set_data(G_OBJECT(source.src_elem), "wedge", GINT_TO_POINTER(1));
  }
  source.bin = source.src_elem;
  gst_bin_add(GST_BIN(app.pipeline.pipeline), source.src_elem);
  gst_element_set_state(app.pipeline.pipeline, GST_STATE_PLAYING);
  const bool stopped = stop_pipeline_gracefully(&app, 0);
  const bool restarted = g_object_get_data(G_OBJECT(source.src_elem), "restarted") != nullptr;
  gst_object_unref(app.pipeline.pipeline);
  g_strfreev(source.uri_list);
  g_cond_clear(&sources.uri_playlist_barrier_cond);
  g_mutex_clear(&sources.uri_playlist_barrier_mutex);
  return stopped && !restarted ? 0 : 1;
}

int run_chapter(bool wedged) {
  g_setenv("HM_SHUTDOWN_WATCHDOG_SECONDS", "1", TRUE);
  auto owned_app = std::make_unique<AppCtx>();
  AppCtx& app = *owned_app;
  auto& sources = app.pipeline.multi_src_bin;
  app.eos_received = TRUE;
  g_mutex_init(&sources.uri_playlist_barrier_mutex);
  g_cond_init(&sources.uri_playlist_barrier_cond);
  sources.uri_playlist_barrier_initialized = TRUE;
  sources.num_bins = 1;
  NvDsSourceConfig config{};
  config.uri = g_strdup("file:///old.mp4");
  auto& source = sources.sub_bins[0];
  source.config = &config;
  source.parent_bin = &sources;
  source.num_uri_list = 2;
  source.uri_list = g_new0(gchar*, 3);
  source.uri_list[0] = g_strdup(config.uri);
  source.uri_list[1] = g_strdup("file:///next.mp4");
  source.src_elem = GST_ELEMENT(g_object_new(stuck_source_get_type(), nullptr));
  source.bin = source.src_elem;
  source.uri_playlist_callbacks_enabled = TRUE;
  g_mutex_init(&source.uri_decode_pad_selection_mutex);
  if (wedged)
    g_object_set_data(G_OBJECT(source.src_elem), "wedge", GINT_TO_POINTER(1));
  app.pipeline.pipeline = gst_pipeline_new(nullptr);
  gst_bin_add(GST_BIN(app.pipeline.pipeline), source.src_elem);
  gst_element_set_state(app.pipeline.pipeline, GST_STATE_PLAYING);

  GstBufferPool* pool = gst_buffer_pool_new();
  GstStructure* pool_config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(pool_config, nullptr, 64, 0, 1);
  gst_buffer_pool_set_config(pool, pool_config);
  gst_buffer_pool_set_active(pool, TRUE);
  GstBuffer* frame = nullptr;
  gst_buffer_pool_acquire_buffer(pool, &frame, nullptr);
  hm::UriPlaylistCapturePoolDrain drain;
  drain.Observe(frame);
  gst_buffer_pool_set_active(pool, FALSE);
  gst_object_unref(pool);
  g_object_set_data(G_OBJECT(source.bin), "hstream-uri-capture-pool-drain", &drain);

  // Simulate the prepare ACK immediately before chapter dispatch. Waiting
  // longer than the synchronous guard must not retire or kill a paused process.
  set_uri_playlist_process_paused(TRUE);
  if (!queue_uri_playlist_switch_callback_for_test(&sources, 0, 1))
    return 1;
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
  while (std::chrono::steady_clock::now() < until) {
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(1000);
  }
  if (source.uri_list_reader_retired)
    return 1;
  set_uri_playlist_process_paused(FALSE);
  until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!source.uri_list_reader_retired && std::chrono::steady_clock::now() < until) {
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(1000);
  }
  if (!source.uri_list_reader_retired || !gst_element_is_locked_state(source.src_elem))
    return 1;
  // A parent pause cannot reopen the old reader while a borrower remains.
  gst_element_set_state(app.pipeline.pipeline, GST_STATE_PAUSED);
  const bool reopened = g_object_get_data(G_OBJECT(source.src_elem), "restarted") != nullptr;
  GstState reader_state = GST_STATE_VOID_PENDING;
  gst_element_get_state(source.src_elem, &reader_state, nullptr, 0);
  const bool held = source.uri_list_index == 0 && reader_state == GST_STATE_NULL && !reopened;
  gst_buffer_unref(frame);
  until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (source.uri_list_index == 0 && std::chrono::steady_clock::now() < until) {
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(1000);
  }
  const bool advanced = source.uri_list_index == 1 && !gst_element_is_locked_state(source.src_elem) &&
      g_strcmp0(
          static_cast<const char*>(g_object_get_data(G_OBJECT(source.src_elem), "test-uri")), source.uri_list[1]) == 0;
  stop_pipeline_gracefully(&app, 0);
  g_object_set_data(G_OBJECT(source.bin), "hstream-uri-capture-pool-drain", nullptr);
  gst_object_unref(app.pipeline.pipeline);
  g_free(config.uri);
  g_strfreev(source.uri_list);
  g_mutex_clear(&source.uri_decode_pad_selection_mutex);
  g_cond_clear(&sources.uri_playlist_barrier_cond);
  g_mutex_clear(&sources.uri_playlist_barrier_mutex);
  return held && advanced ? 0 : 1;
}

bool child_case(bool wedged, const char* seconds, bool should_exit, int expected_exit, bool chapter = false) {
  const pid_t child = fork();
  if (child == 0) {
    _exit(chapter ? run_chapter(wedged) : run_stop(wedged, seconds));
  }
  if (child < 0) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(child, &status, WNOHANG) == child) {
      const bool okay = should_exit && WIFEXITED(status) && WEXITSTATUS(status) == expected_exit;
      if (!okay) {
        std::cerr << "wedged=" << wedged << " seconds=" << seconds << " status=" << status << "\n";
      }
      return okay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  if (should_exit) {
    std::cerr << "Child exceeded its stop deadline: wedged=" << wedged << " seconds=" << seconds << "\n";
  }
  return !should_exit;
}

bool asynchronous_resume_restores_monitoring() {
  auto owned_app = std::make_unique<AppCtx>();
  AppCtx& app = *owned_app;
  auto& sources = app.pipeline.multi_src_bin;
  g_mutex_init(&sources.uri_playlist_barrier_mutex);
  g_cond_init(&sources.uri_playlist_barrier_cond);
  sources.uri_playlist_barrier_initialized = TRUE;
  app.pipeline.pipeline = GST_ELEMENT(g_object_new(stuck_source_get_type(), nullptr));
  gst_element_set_state(app.pipeline.pipeline, GST_STATE_PAUSED);
  set_uri_playlist_waits_paused(&sources, TRUE);
  g_object_set_data(G_OBJECT(app.pipeline.pipeline), "async-play", GINT_TO_POINTER(1));
  const bool resumed = resume_pipeline(&app);
  const bool monitoring = !sources.uri_playlist_waits_paused;
  gst_element_set_state(app.pipeline.pipeline, GST_STATE_NULL);
  gst_object_unref(app.pipeline.pipeline);
  g_cond_clear(&sources.uri_playlist_barrier_cond);
  g_mutex_clear(&sources.uri_playlist_barrier_mutex);
  return !resumed && monitoring;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  if (!child_case(false, "1", true, 0) || !child_case(true, "1", true, 70) || !child_case(true, "0", false, 0) ||
      !child_case(false, "1", true, 0, true) || !child_case(true, "1", true, 70, true) ||
      !asynchronous_resume_restores_monitoring()) {
    std::cerr << "Graceful stop watchdog did not cover source retirement or honor its override\n";
    return 1;
  }
  return 0;
}
