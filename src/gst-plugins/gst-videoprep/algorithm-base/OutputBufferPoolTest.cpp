#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/OutputBufferPool.h"

#include <iostream>
#include <string>

namespace {

struct PoolState {
  int allocated{0};
  int freed{0};
  int finalized{0};
};

struct TestOutputPool {
  GstBufferPool parent;
  PoolState* state;
  bool reject_config;
  bool fail_allocation;
};
struct TestOutputPoolClass {
  GstBufferPoolClass parent;
};
G_DEFINE_TYPE(TestOutputPool, test_output_pool, GST_TYPE_BUFFER_POOL)

gboolean set_config(GstBufferPool* pool, GstStructure* config) {
  if (reinterpret_cast<TestOutputPool*>(pool)->reject_config)
    return FALSE;
  return GST_BUFFER_POOL_CLASS(test_output_pool_parent_class)->set_config(pool, config);
}

GstFlowReturn allocate(GstBufferPool* pool, GstBuffer** buffer, GstBufferPoolAcquireParams* params) {
  auto* test = reinterpret_cast<TestOutputPool*>(pool);
  if (test->fail_allocation)
    return GST_FLOW_ERROR;
  const auto flow = GST_BUFFER_POOL_CLASS(test_output_pool_parent_class)->alloc_buffer(pool, buffer, params);
  if (flow == GST_FLOW_OK)
    ++test->state->allocated;
  return flow;
}

void free_buffer(GstBufferPool* pool, GstBuffer* buffer) {
  ++reinterpret_cast<TestOutputPool*>(pool)->state->freed;
  GST_BUFFER_POOL_CLASS(test_output_pool_parent_class)->free_buffer(pool, buffer);
}

void test_output_pool_class_init(TestOutputPoolClass* type) {
  auto* pool = GST_BUFFER_POOL_CLASS(type);
  pool->set_config = set_config;
  pool->alloc_buffer = allocate;
  pool->free_buffer = free_buffer;
}
void test_output_pool_init(TestOutputPool*) {}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

GstBufferPool* make_pool(PoolState& state, bool reject_config, bool fail_allocation) {
  auto* test = reinterpret_cast<TestOutputPool*>(g_object_new(test_output_pool_get_type(), nullptr));
  test->state = &state;
  test->reject_config = reject_config;
  test->fail_allocation = fail_allocation;
  g_object_weak_ref(
      G_OBJECT(test), [](gpointer data, GObject*) { ++static_cast<PoolState*>(data)->finalized; }, &state);
  return GST_BUFFER_POOL(test);
}

GstStructure* configuration(GstBufferPool* pool) {
  GstStructure* config = gst_buffer_pool_get_config(pool);
  GstCaps* caps = gst_caps_from_string("video/x-raw,format=RGBA,width=15790,height=6057");
  // Only the metadata is large: the allocator regression uses tiny CPU buffers.
  gst_buffer_pool_config_set_params(config, caps, 16, 2, 6);
  gst_structure_set(config, "batch-size", G_TYPE_UINT, 1, nullptr);
  gst_caps_unref(caps);
  return config;
}

bool failure_releases_pool(bool reject_config) {
  PoolState state;
  GstBufferPool* pool = make_pool(state, reject_config, !reject_config);
  const auto* result = hm::videoprep::configure_output_pool(pool, configuration(pool));
  return expect(result == nullptr, "failed pool configuration/activation must not return a usable pool") &&
      expect(state.finalized == 1, "failed setup must release the pool's final reference") &&
      expect(state.allocated == 0, "the activation fixture fails before its first allocation") &&
      expect(state.allocated == state.freed, "failed setup must not acquire a buffer");
}

bool diagnostics_preserve_context() {
  PoolState state;
  GstBufferPool* pool = make_pool(state, false, false);
  pool = hm::videoprep::configure_output_pool(pool, configuration(pool));
  if (!expect(pool != nullptr && state.finalized == 0, "successful setup must transfer an active pool"))
    return false;
  constexpr uint64_t kMiB = 1024ULL * 1024ULL;
  const auto failure = hm::videoprep::output_buffer_failure_status(
      pool, GST_FLOW_ERROR, 3, hm::videoprep::OutputGpuMemory{5922 * kMiB, 24564 * kMiB}, "");
  const std::string message(failure.message());
  bool ok = expect(absl::IsInternal(failure), "a generic flow failure must not be mislabeled as proven GPU OOM");
  for (const char* detail :
       {"error (-5)",
        "15790x6057 RGBA",
        "minimum 2, maximum 6 buffers",
        "batch size 1",
        "GPU 3 memory 5922 MiB free of 24564 MiB"})
    ok &= expect(message.find(detail) != std::string::npos, detail);
  const auto unavailable = hm::videoprep::output_buffer_failure_status(
      pool, GST_FLOW_NOT_NEGOTIATED, 3, std::nullopt, "device query failed");
  ok &= expect(
      unavailable.message().find("not-negotiated") != std::string::npos &&
          unavailable.message().find("memory unavailable (device query failed)") != std::string::npos,
      "a failed memory query must preserve the flow error without reporting fabricated zero free memory");
  gst_buffer_pool_set_active(pool, FALSE);
  gst_object_unref(pool);
  ok &= expect(state.finalized == 1 && state.allocated == state.freed, "successful pool cleanup must release buffers");
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool ok = failure_releases_pool(true);
  ok &= failure_releases_pool(false);
  ok &= diagnostics_preserve_context();
  return ok ? 0 : 1;
}
