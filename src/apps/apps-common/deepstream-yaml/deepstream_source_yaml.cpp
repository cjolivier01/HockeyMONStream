#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/apps/apps-common/deepstream_config_yaml.h"
#include "hstream/src/libs/common/ConfigYaml.h"

#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>

using std::cout;
using std::endl;

#define N_DECODE_SURFACES 16
#define N_EXTRA_SURFACES 1

#if 1

using std::cout;
using std::endl;

#define N_DECODE_SURFACES 16
#define N_EXTRA_SURFACES 1

// New-style parser using ConfigYaml approach.
gboolean parse_source_yaml(NvDsSourceConfig* config, const YAML::Node& yaml_node, const gchar* cfg_file_path) {
  // Set default values.
  config->latency = 100;
  config->num_decode_surfaces = N_DECODE_SURFACES;
  config->num_extra_surfaces = N_EXTRA_SURFACES;

  // Create a configuration locator.
  hm::utils::ConfigLocator locator;

  // Set numeric fields.
  SET_LOCATOR_ENUM(locator, *config, type, NvDsSourceType); // "type"
  SET_LOCATOR(locator, *config, enable); // "enable"
  SET_LOCATOR(locator, *config, camera_width); // "camera-width" (dash replaced to underscore)
  SET_LOCATOR(locator, *config, camera_height); // "camera-height"
  SET_LOCATOR(locator, *config, camera_fps_n); // "camera-fps-n"
  SET_LOCATOR(locator, *config, camera_fps_d); // "camera-fps-d"
  SET_LOCATOR(locator, *config, camera_csi_sensor_id); // "camera-csi-sensor-id"
  SET_LOCATOR(locator, *config, camera_i2c_bus); // "camera-i2c-bus"
  SET_LOCATOR(locator, *config, camera_wbmode);  // "camera-wbmode"
  SET_LOCATOR(locator, *config, camera_auto_focus); // "camera-auto-focus"
  SET_LOCATOR(locator, *config, camera_saturation);
  SET_LOCATOR(locator, *config, camera_exposure_compensation);
  SET_LOCATOR(locator, *config, camera_v4l2_dev_node); // "camera-v4l2-dev-node"
  SET_LOCATOR(locator, *config, udp_buffer_size); // "udp-buffer-size"
  SET_LOCATOR(locator, *config, flip_method); // "flip-method"
  SET_LOCATOR(locator, *config, num_sources); // "num-sources"
  SET_LOCATOR(locator, *config, gpu_id); // "gpu-id"
  SET_LOCATOR(locator, *config, num_decode_surfaces); // "num-decode-surfaces"
  SET_LOCATOR(locator, *config, num_extra_surfaces); // "num-extra-surfaces"
  SET_LOCATOR(locator, *config, drop_frame_interval); // "drop-frame-interval"
  SET_LOCATOR(locator, *config, camera_id); // "camera-id"
  SET_LOCATOR(locator, *config, input_audio_rate); // "input-audio-rate" or "audio-input-rate"
  SET_LOCATOR(locator, *config, rtsp_reconnect_interval_sec); // "rtsp-reconnect-interval-sec"
  SET_LOCATOR(locator, *config, rtsp_reconnect_attempts); // "rtsp-reconnect-attempts"
  SET_LOCATOR(locator, *config, intra_decode_enable); // "intra-decode-enable"
  SET_LOCATOR(locator, *config, cuda_memory_type); // "cuda_memory_type"
  SET_LOCATOR(locator, *config, nvbuf_memory_type); // "nvbuf-memory-type"
  SET_LOCATOR(locator, *config, select_rtp_protocol); // "select-rtp-protocol"
  SET_LOCATOR(locator, *config, source_id); // "source-id"
  SET_LOCATOR(locator, *config, smart_record); // "smart-record"
  SET_LOCATOR(locator, *config, smart_rec_cache_size); // "smart-rec-cache" (or deprecated "smart-rec-video-cache")
  SET_LOCATOR(locator, *config, smart_rec_container); // "smart-rec-container"
  SET_LOCATOR(locator, *config, smart_rec_start_time); // "smart-rec-start-time"
  SET_LOCATOR(locator, *config, smart_rec_def_duration); // "smart-rec-default-duration"
  SET_LOCATOR(locator, *config, smart_rec_duration); // "smart-rec-duration"
  SET_LOCATOR(locator, *config, smart_rec_interval); // "smart-rec-interval"
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  SET_LOCATOR(locator, *config, nvvideoconvert_copy_hw); // "copy-hw"
#endif

  SET_LOCATOR_CHAR_PTR(locator, *config, exposure_time_range);
  SET_LOCATOR_CHAR_PTR(locator, *config, gain_range);

  // Set string fields as character arrays (assumes members are fixed-size arrays).
  SET_LOCATOR_CHAR_PTR(locator, *config, alsa_device); // "alsa-device"
  SET_LOCATOR_CHAR_PTR(locator, *config, video_format); // "video-format"
  SET_LOCATOR_CHAR_PTR(locator, *config, media_type); // "media-type"
  SET_LOCATOR_CHAR_PTR(locator, *config, uri); // "uri"
  SET_LOCATOR_CHAR_PTR(locator, *config, start_rec_dir_path); // "smart-rec-dir-path"
  SET_LOCATOR_CHAR_PTR(locator, *config, start_rec_file_prefix); // "smart-rec-file-prefix"

  // Use the new YAML parser to set config values.
  hm::utils::set_config_from_yaml(yaml_node, locator);

  // Special handling for the "uri" field.
  if (config->uri && g_str_has_prefix(config->uri, "file://")) {
    // Remove the "file://" prefix and get the absolute file path.
    const char* filePart = config->uri + 7;
    char absolutePath[1024] = {0};
    get_absolute_file_path_yaml(cfg_file_path, filePart, absolutePath);
    // Update the URI using the new absolute path.
    config->uri = g_strdup_printf("file://%s", absolutePath);
  }

  // Validate directory path for smart recording.
  if (config->start_rec_dir_path) {
    if (access(config->start_rec_dir_path, 2)) {
      if (errno == ENOENT || errno == ENOTDIR) {
        g_print("ERROR: Directory (%s) doesn't exist.\n", config->start_rec_dir_path);
      } else if (errno == EACCES) {
        g_print("ERROR: No write permission in %s\n", config->start_rec_dir_path);
      }
      return FALSE;
    }
  }

  return TRUE;
}

#else

gboolean parse_source_yaml(
    NvDsSourceConfig* config,
    std::vector<std::string> headers,
    std::vector<std::string> source_values,
    const gchar* cfg_file_path) {
  gboolean ret = FALSE;

  config->latency = 100;
  config->num_decode_surfaces = N_DECODE_SURFACES;
  config->num_extra_surfaces = N_EXTRA_SURFACES;

  for (unsigned int i = 0; i < headers.size(); i++) {
    std::string paramKey = headers[i];

    if (paramKey == "type") {
      gint temp = std::stoi(source_values[i]);
      config->type = (NvDsSourceType)temp;
    } else if (paramKey == "enable") {
      config->enable = std::stoul(source_values[i]);
    } else if (paramKey == "camera-width") {
      config->source_width = std::stoi(source_values[i]);
    } else if (paramKey == "camera-height") {
      config->source_height = std::stoi(source_values[i]);
    } else if (paramKey == "camera-fps-n") {
      config->source_fps_n = std::stoi(source_values[i]);
    } else if (paramKey == "camera-fps-d") {
      config->source_fps_d = std::stoi(source_values[i]);
    } else if (paramKey == "camera-csi-sensor-id") {
      config->camera_csi_sensor_id = std::stoi(source_values[i]);
    } else if (paramKey == "camera-i2c-bus") {
      config->camera_i2c_bus = std::stoi(source_values[i]);
    } else if (paramKey == "camera-auto-focus") {
      config->camera_auto_focus = std::stoi(source_values[i]);
    } else if (paramKey == "camera-v4l2-dev-node") {
      config->camera_v4l2_dev_node = std::stoi(source_values[i]);
    } else if (paramKey == "udp-buffer-size") {
      config->udp_buffer_size = std::stoi(source_values[i]);
    } else if (paramKey == "alsa-device") {
      std::string temp = source_values[i];
      config->alsa_device = (char*)malloc(sizeof(char) * 1024);
      std::strncpy(config->alsa_device, temp.c_str(), 1023);
    } else if (paramKey == "video-format") {
      std::string temp = source_values[i];
      config->video_format = (char*)malloc(sizeof(char) * 1024);
      std::strncpy(config->video_format, temp.c_str(), 1023);
    } else if (paramKey == "media-type") {
      std::string temp = source_values[i];
      config->media_type = (char*)malloc(sizeof(char) * 1024);
      std::strncpy(config->media_type, temp.c_str(), 1023);
    } else if (paramKey == "uri") {
      std::string temp = source_values[i];
      char* uri = (char*)malloc(sizeof(char) * 1024);
      std::strncpy(uri, temp.c_str(), 1023);
      char* str;
      if (g_str_has_prefix(uri, "file://")) {
        str = g_strdup(uri + 7);
        config->uri = (char*)malloc(sizeof(char) * 1024);
        get_absolute_file_path_yaml(cfg_file_path, str, config->uri);
        config->uri = g_strdup_printf("file://%s", config->uri);
        g_free(uri);
        g_free(str);
      } else {
        config->uri = uri;
      }
    } else if (paramKey == "latency") {
      config->latency = std::stoi(source_values[i]);
    } else if (paramKey == "num-sources") {
      config->num_sources = std::stoul(source_values[i]);
      if (config->num_sources < 1) {
        config->num_sources = 1;
      }
    } else if (paramKey == "gpu-id") {
      config->gpu_id = std::stoul(source_values[i]);
    } else if (paramKey == "num-decode-surfaces") {
      config->num_decode_surfaces = std::stoul(source_values[i]);
    } else if (paramKey == "num-extra-surfaces") {
      config->num_extra_surfaces = std::stoul(source_values[i]);
    } else if (paramKey == "drop-frame-interval") {
      config->drop_frame_interval = std::stoul(source_values[i]);
    } else if (paramKey == "camera-id") {
      config->camera_id = std::stoul(source_values[i]);
    } else if (paramKey == "input-audio-rate" || paramKey == "audio-input-rate") {
      config->input_audio_rate = std::stoul(source_values[i]);
    } else if (paramKey == "rtsp-reconnect-interval-sec") {
      config->rtsp_reconnect_interval_sec = std::stoi(source_values[i]);
    } else if (paramKey == "rtsp-reconnect-attempts") {
      config->rtsp_reconnect_attempts = std::stoul(source_values[i]);
    } else if (paramKey == "intra-decode-enable") {
      config->intra_decode_enable = (gboolean)std::stoul(source_values[i]);
    } else if (paramKey == "cuda_memory_type") {
      config->cuda_memory_type = std::stoul(source_values[i]);
    } else if (paramKey == "nvbuf-memory-type") {
      config->nvbuf_memory_type = std::stoul(source_values[i]);
    } else if (paramKey == "select-rtp-protocol") {
      config->select_rtp_protocol = std::stoul(source_values[i]);
    } else if (paramKey == "source-id") {
      config->source_id = std::stoul(source_values[i]);
    } else if (paramKey == "smart-record") {
      config->smart_record = std::stoul(source_values[i]);
    } else if (paramKey == "smart-rec-dir-path") {
      std::string temp = source_values[i];
      config->dir_path = (char*)malloc(sizeof(char) * 1024);
      std::strncpy(config->dir_path, temp.c_str(), 1023);

      if (access(config->dir_path, 2)) {
        if (errno == ENOENT || errno == ENOTDIR) {
          g_print("ERROR: Directory (%s) doesn't exist.\n", config->dir_path);
        } else if (errno == EACCES) {
          g_print("ERROR: No write permission in %s\n", config->dir_path);
        }
        goto done;
      }
    } else if (paramKey == "smart-rec-file-prefix") {
      std::string temp = source_values[i];
      config->file_prefix = (char*)malloc(sizeof(char) * 1024);
      std::strncpy(config->file_prefix, temp.c_str(), 1023);
    } else if (paramKey == "smart-rec-video-cache") {
      cout << "Deprecated config smart-rec-video-cache used in source. Use smart-rec-cache instead" << endl;

      config->smart_rec_cache_size = std::stoul(source_values[i]);
    } else if (paramKey == "smart-rec-cache") {
      config->smart_rec_cache_size = std::stoul(source_values[i]);
    } else if (paramKey == "smart-rec-container") {
      config->smart_rec_container = std::stoul(source_values[i]);
    } else if (paramKey == "smart-rec-start-time") {
      config->smart_rec_start_time = std::stoul(source_values[i]);
    } else if (paramKey == "smart-rec-default-duration") {
      config->smart_rec_def_duration = std::stoul(source_values[i]);
    } else if (paramKey == "smart-rec-duration") {
      config->smart_rec_duration = std::stoul(source_values[i]);
    } else if (paramKey == "smart-rec-interval") {
      config->smart_rec_interval = std::stoul(source_values[i]);
    }
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
    else if (paramKey == "copy-hw") {
      config->nvvideoconvert_copy_hw = std::stoul(source_values[i]);
    }
#endif
    else {
      cout << "[WARNING] Unknown param found in source : " << paramKey << endl;
    }
  }

  ret = TRUE;
done:
  if (!ret) {
    cout << __func__ << " failed" << endl;
  }
  return ret;
}
#endif
