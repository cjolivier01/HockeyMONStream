#include "hstream/src/apps/apps-common/deepstream_config_file_parser.h"
#include "hstream/src/apps/apps-common/deepstream_config_yaml.h"

#include <gst/gst.h>
#include <nvbufsurftransform.h>

#include <iostream>
#include <string>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

bool parses_as(const std::string& value, gint expected) {
  YAML::Node root;
  root["sink6"]["enable"] = 1;
  root["sink6"]["interpolation-method"] = value;
  NvDsSinkSubBinConfig config{};
  return parse_sink_yaml(&config, "sink6", root, ".") && config.encoder_config.interpolation_method_set &&
      config.encoder_config.interpolation_method == expected;
}

bool parses_ini_as(const std::string& value, gint expected) {
  GKeyFile* key_file = g_key_file_new();
  g_key_file_set_integer(key_file, "sink6", "enable", 1);
  g_key_file_set_string(key_file, "sink6", "interpolation-method", value.c_str());
  NvDsSinkSubBinConfig config{};
  gchar group[] = "sink6";
  const bool ok = parse_sink(&config, key_file, group, ".") && config.encoder_config.interpolation_method_set &&
      config.encoder_config.interpolation_method == expected;
  g_key_file_unref(key_file);
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

  if (!parses_as("default", NvBufSurfTransformInter_Default) ||
      !parses_as("nearest", NvBufSurfTransformInter_Nearest) ||
      !parses_as("bilinear", NvBufSurfTransformInter_Bilinear) || !parses_as("algo1", NvBufSurfTransformInter_Algo1) ||
      !parses_as("cubic", NvBufSurfTransformInter_Algo1) || !parses_as("bicubic", NvBufSurfTransformInter_Algo1) ||
      !parses_as("super", NvBufSurfTransformInter_Algo2) || !parses_as("lanczos", NvBufSurfTransformInter_Algo3) ||
      !parses_as("nicest", NvBufSurfTransformInter_Algo4) || !parses_as("6", NvBufSurfTransformInter_Default)) {
    std::cerr << "A supported sink interpolation method did not parse\n";
    return 1;
  }

  YAML::Node invalid;
  invalid["sink6"]["enable"] = 1;
  invalid["sink6"]["interpolation-method"] = "bogus";
  NvDsSinkSubBinConfig invalid_config{};
  if (parse_sink_yaml(&invalid_config, "sink6", invalid, ".")) {
    std::cerr << "An invalid sink interpolation method was accepted\n";
    return 1;
  }

  if (!parses_ini_as("algo3", NvBufSurfTransformInter_Algo3) || !parses_ini_as("4", NvBufSurfTransformInter_Algo3)) {
    std::cerr << "A supported INI sink interpolation method did not parse\n";
    return 1;
  }
  return 0;
}
