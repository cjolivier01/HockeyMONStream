#include "hstream/src/apps/apps-common/deepstream_config_yaml.h"
#include "hstream/src/apps/apps-common/deepstream_primary_gie.h"

#include <glib/gstdio.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

struct InferenceFile {
  gchar* path{};
  InferenceFile() {
    const int fd = g_file_open_tmp("primary-gie-XXXXXX.yaml", &path, nullptr);
    Check(fd >= 0, "Cannot create inference YAML");
    close(fd);
    Check(
        g_file_set_contents(
            path,
            "property:\n  network-type: 0\n  gie-unique-id: 1\n  batch-size: 1\n"
            "  num-detected-classes: 1\n  onnx-file: /unused-setup-test.onnx\n",
            -1,
            nullptr),
        "Cannot write inference YAML");
  }
  ~InferenceFile() {
    g_unlink(path);
    g_free(path);
  }
};

struct Setup {
  NvDsGieConfig config{};
  NvDsPrimaryGieBin gie{};
  ~Setup() {
    // Failed construction can leave elements outside the bin.
    for (auto* element : {gie.queue, gie.nvvidconv, gie.capsfilter, gie.primary_gie})
      if (element && !GST_OBJECT_PARENT(element))
        gst_object_unref(element);
    if (gie.bin)
      gst_object_unref(gie.bin);
    g_free(config.config_file_path);
    if (config.bbox_border_color_table)
      g_hash_table_unref(config.bbox_border_color_table);
    if (config.bbox_bg_color_table)
      g_hash_table_unref(config.bbox_bg_color_table);
  }
};

void CheckSetup(const char* inference_file, int tiles, bool enabled, int batch, bool accepted) {
  auto node = YAML::Load(
      "enable: 1\nplugin-type: 0\ninterval: 0\n"
      "tiled-inference: {enable: 1, overlap: 0.2, merge-iou: 0.5}\n");
  node["config-file"] = inference_file;
  node["batch-size"] = batch;
  if (tiles)
    node["tiled-inference"]["tiles"] = tiles; // Retired quadrant preference.
  node["tiled-inference"]["enable"] = int(enabled);
  Setup setup;
  Check(parse_gie_yaml(&setup.config, "primary-gie", node, "."), "Production GIE parser failed");
  Check(bool(create_primary_gie_bin(&setup.config, &setup.gie)) == accepted, "Wrong production GIE setup result");
  if (!accepted)
    return;
  gint process_mode = 0;
  guint actual_batch = 0;
  g_object_get(setup.gie.primary_gie, "process-mode", &process_mode, "batch-size", &actual_batch, nullptr);
  Check(process_mode == (enabled ? 2 : 1), "Wrong inference mode");
  Check(actual_batch == guint(batch), "Wrong inference batch");
  if (enabled) {
    gchar* classes{};
    g_object_get(setup.gie.primary_gie, "infer-on-class-ids", &classes, nullptr);
    const bool correct = g_strcmp0(classes, "0:1:") == 0;
    g_free(classes);
    Check(correct, "Production setup dropped tile selection");
  }
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  try {
    InferenceFile inference;
    CheckSetup(inference.path, 0, false, 1, true);
    CheckSetup(inference.path, 0, true, 2, true);
    CheckSetup(inference.path, 2, true, 2, true);
    CheckSetup(inference.path, 4, true, 4, false);
    CheckSetup(inference.path, 4, true, 2, true);
    CheckSetup(inference.path, 0, true, 1, false);
    CheckSetup(inference.path, 2, true, 4, false);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
