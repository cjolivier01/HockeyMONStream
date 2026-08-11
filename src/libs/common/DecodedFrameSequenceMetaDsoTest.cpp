#include <dlfcn.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using Initialize = bool (*)();
using Add = bool (*)(GstBuffer*, guint, guint64);
using Read = bool (*)(GstBuffer*, guint*, guint64*);

struct DsoFunctions {
  void* handle{nullptr};
  Initialize initialize{nullptr};
  Add add{nullptr};
  Read read{nullptr};
};

std::string runfile(const char* file) {
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  if (!test_srcdir || !test_workspace) {
    return {};
  }
  return std::string(test_srcdir) + "/" + test_workspace + "/src/libs/common/" + file;
}

DsoFunctions load_dso(const char* file) {
  DsoFunctions functions;
  const std::string path = runfile(file);
  functions.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!functions.handle) {
    std::cerr << "Could not load " << path << ": " << dlerror() << std::endl;
    return functions;
  }
  functions.initialize =
      reinterpret_cast<Initialize>(dlsym(functions.handle, "decoded_frame_sequence_meta_dso_initialize"));
  functions.add = reinterpret_cast<Add>(dlsym(functions.handle, "decoded_frame_sequence_meta_dso_add"));
  functions.read = reinterpret_cast<Read>(dlsym(functions.handle, "decoded_frame_sequence_meta_dso_read"));
  return functions;
}

bool interoperates(const DsoFunctions& writer, const DsoFunctions& reader, guint source_id, guint64 sequence) {
  GstBuffer* buffer = gst_buffer_new();
  if (!writer.add(buffer, source_id, sequence)) {
    gst_buffer_unref(buffer);
    return false;
  }
  GstBuffer* copy = gst_buffer_copy_deep(buffer);
  gst_buffer_unref(buffer);

  guint actual_source_id = 0;
  guint64 actual_sequence = 0;
  const bool read = reader.read(copy, &actual_source_id, &actual_sequence);
  gst_buffer_unref(copy);
  return read && actual_source_id == source_id && actual_sequence == sequence;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  const DsoFunctions first = load_dso("decoded_frame_sequence_meta_dso_a.so");
  const DsoFunctions second = load_dso("decoded_frame_sequence_meta_dso_b.so");
  if (!first.handle || !second.handle || !first.initialize || !first.add || !first.read || !second.initialize ||
      !second.add || !second.read) {
    return 1;
  }

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  bool first_initialized = false;
  bool second_initialized = false;
  auto initialize = [&](const DsoFunctions& functions, bool& result) {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    result = functions.initialize();
  };

  std::thread first_thread(initialize, std::cref(first), std::ref(first_initialized));
  std::thread second_thread(initialize, std::cref(second), std::ref(second_initialized));
  while (ready.load(std::memory_order_acquire) != 2) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();

  if (!first_initialized || !second_initialized || !interoperates(first, second, 7, 1234) ||
      !interoperates(second, first, 9, 4321)) {
    std::cerr << "Independently linked metadata implementations did not register and interoperate safely\n";
    return 1;
  }
  return 0;
}
