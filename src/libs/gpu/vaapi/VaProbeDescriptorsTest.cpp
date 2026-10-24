#include <gst/gst.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay_drm.h>
#include <va/va_drmcommon.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "hstream/src/libs/gpu/Runtime.h"

namespace {
enum class ExportMode { kNormal, kDriverFailure, kInvalidLayout };
ExportMode mode = ExportMode::kNormal;
std::array<int, 4> exported_fds{};
unsigned exported_count = 0;
bool injection_ok = true;
} // namespace

// Interpose only this test executable's libva calls. A failed Mesa export can
// leave already-closed descriptor numbers in the returned structure. Reuse
// those numbers deliberately to detect cleanup that wrongly assumes ownership.
extern "C" VAStatus vaExportSurfaceHandle(
    VADisplay display,
    VASurfaceID surface,
    uint32_t memory_type,
    uint32_t flags,
    void* descriptor) {
  using Function = VAStatus (*)(VADisplay, VASurfaceID, uint32_t, uint32_t, void*);
  static const auto original = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "vaExportSurfaceHandle"));
  if (!original)
    return VA_STATUS_ERROR_OPERATION_FAILED;
  const auto status = original(display, surface, memory_type, flags, descriptor);
  if (status != VA_STATUS_SUCCESS || mode == ExportMode::kNormal)
    return status;
  auto* desc = static_cast<VADRMPRIMESurfaceDescriptor*>(descriptor);
  exported_count = desc->num_objects;
  if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 || !exported_count ||
      exported_count > exported_fds.size()) {
    injection_ok = false;
    return status;
  }
  for (unsigned i = 0; i < exported_count; ++i)
    exported_fds[i] = desc->objects[i].fd;
  if (mode == ExportMode::kInvalidLayout) {
    desc->num_layers = 0;
    return VA_STATUS_SUCCESS;
  }
  // Open before closing any export so the replacement backing fd cannot
  // coincide with one of the exported numbers.
  const int backing = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (backing < 0) {
    injection_ok = false;
    return status;
  }
  for (unsigned i = 0; i < exported_count; ++i) {
    close(exported_fds[i]);
    if (dup2(backing, exported_fds[i]) != exported_fds[i])
      injection_ok = false;
  }
  close(backing);
  return VA_STATUS_ERROR_OPERATION_FAILED;
}

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
size_t Descriptors() {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
    (void)entry, ++count;
  return count;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    char pci[32]{};
    Check(hipDeviceGetPCIBusId(pci, sizeof(pci), 0) == hipSuccess, "Missing HIP device");
    const auto render = std::filesystem::canonical(std::string("/dev/dri/by-path/pci-") + pci + "-render");
    std::unique_ptr<GstVaDisplay, decltype(&gst_object_unref)> display(
        gst_va_display_drm_new_from_path(render.c_str()), gst_object_unref);
    Check(bool(display), "Cannot open VA display");
    const auto probe = [&] {
      return gst_va_dmabuf_get_modifier_for_format(
          display.get(), GST_VIDEO_FORMAT_NV12, VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC);
    };
    (void)probe();
    const auto before = Descriptors();
    for (int i = 0; i < 32; ++i)
      (void)probe();
    Check(Descriptors() == before, "Successful modifier probes leaked descriptors");

    mode = ExportMode::kDriverFailure;
    (void)probe();
    Check(injection_ok && exported_count, "Failed to inject driver failure");
    bool preserved = true;
    for (unsigned i = 0; i < exported_count; ++i) {
      preserved &= fcntl(exported_fds[i], F_GETFD) >= 0;
      close(exported_fds[i]);
    }
    Check(preserved, "Failed export cleanup closed descriptors reused by another owner");

    mode = ExportMode::kInvalidLayout;
    exported_count = 0;
    (void)probe();
    Check(injection_ok && exported_count, "Failed to inject unsupported exported layout");
    for (unsigned i = 0; i < exported_count; ++i)
      Check(fcntl(exported_fds[i], F_GETFD) < 0, "Rejected successful export leaked descriptors");
    Check(Descriptors() == before, "Descriptor ownership checks leaked resources");
    std::cout << "VA modifier probe success, driver failure and rejected-layout ownership passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
