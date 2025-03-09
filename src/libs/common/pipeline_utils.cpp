#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/src/libs/common/utils.h"

#include <fstream>
#include <iostream>
#include <vector>

#include <opencv2/opencv.hpp>
#include "absl/strings/str_split.h"

namespace hm {

Videoinfo getVideoInfo(const std::string& videoPath) {
  Videoinfo info;
  // Open the video file.
  cv::VideoCapture cap(videoPath);
  if (!cap.isOpened()) {
    std::cerr << "Error: Could not open video file: " << videoPath << std::endl;
    return info;
  }

  // Retrieve the FPS property.
  info.fps = cap.get(cv::CAP_PROP_FPS);
  info.frame_count = cap.get(cv::CAP_PROP_FRAME_COUNT);
  info.width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
  info.height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
  info.video_bit_rate = cap.get(cv::CAP_PROP_BITRATE) * 1000;
  info.audio_samples_per_second = cap.get(cv::CAP_PROP_AUDIO_SAMPLES_PER_SECOND);
  info.num_audio_channels = cap.get(cv::CAP_PROP_AUDIO_TOTAL_CHANNELS);
  info.num_audio_streams = cap.get(cv::CAP_PROP_AUDIO_TOTAL_STREAMS);
  return info;
}

bool has_node(const YAML::Node& n, const std::string& dot_string, bool non_null) {
  if (dot_string.empty()) {
    return true;
  }
  std::vector<std::string> keys = absl::StrSplit(dot_string, '.');

  // Start from the root node
  const YAML::Node* current = &n;

  // Traverse the node tree according to the keys
  for (const auto& key : keys) {
    if (!current->IsMap()) {
      // If the current node is not a map, no further keys can be accessed
      return false;
    }

    // Access the next node in the path
    auto tmp = (*current)[key];
    current = &tmp;

    // Check if the current node is defined
    if (!current->IsDefined()) {
      return false;
    }
  }

  // If all keys are accessed and nodes are defined, return true
  return non_null ? !current->IsNull() : true;
}

std::optional<YAML::Node> get_node(const YAML::Node& n, const std::string& dot_string) {
  if (dot_string.empty()) {
    return n;
  }
  std::vector<std::string> keys = absl::StrSplit(dot_string, '.');

  // Start from the root node
  const YAML::Node* current = &n;

  // Traverse the node tree according to the keys
  for (const auto& key : keys) {
    if (!current->IsMap()) {
      // If the current node is not a map, no further keys can be accessed
      return std::nullopt;
    }

    // Access the next node in the path
    auto tmp = (*current)[key];
    current = &tmp;

    // Check if the current node is defined
    if (!current->IsDefined()) {
      return std::nullopt;
    }
  }

  // If all keys are accessed and nodes are defined, return true
  return *current;
}

bool seek_element(GstElement* seek_element, size_t seek_to_nanoseconds) {
  // size_t seekTarget = static_cast<size_t>(abs_seconds * 60 * GST_SECOND);
  size_t seekTarget = seek_to_nanoseconds;
  if (!gst_element_seek(
          seek_element,
          1.0, // Normal playback rate.
          GST_FORMAT_TIME,
          // (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_ACCURATE),
          (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_ACCURATE),
          GST_SEEK_TYPE_SET, // Start from the target position.
          seekTarget,
          GST_SEEK_TYPE_NONE, // No specific end position.
          GST_CLOCK_TIME_NONE)) {
    g_printerr("Seek failed\n");
    return false;
  } else {
    g_print("Seek successful to %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(seekTarget));
    return true;
  }
}

void save_dot_file(GstElement* pipeline, GstDebugGraphDetails details, const std::string& filename) {
  gchar* dot_data = gst_debug_bin_to_dot_data(GST_BIN(pipeline), details);
  if (dot_data) {
    // Print to stdout
    // std::cout << dot_data << std::endl;

    // Or save to a file
    std::ofstream dot_file(TO_STRING(filename << ".dot"));
    if (dot_file.is_open()) {
      dot_file << dot_data;
      dot_file.close();
      std::cout << "DOT file saved as '" << filename << ".dot'" << std::endl;
    } else {
      std::cerr << "Failed to open file for writing." << std::endl;
    }

    g_free(dot_data);
  } else {
    std::cerr << "Failed to generate DOT data" << std::endl;
  }
}

const char* gstStateToString(GstState state) {
  switch (state) {
    case GST_STATE_VOID_PENDING:
      return "VOID_PENDING";
    case GST_STATE_NULL:
      return "NULL";
    case GST_STATE_READY:
      return "READY";
    case GST_STATE_PAUSED:
      return "PAUSED";
    case GST_STATE_PLAYING:
      return "PLAYING";
    default:
      return "UNKNOWN";
  }
}

void waitForPipelineStop(GstElement* pipeline) {
  // Get the pipeline's bus
  GstBus* bus = gst_element_get_bus(pipeline);

  // Record the time of the last message print
  auto lastPrintTime = std::chrono::steady_clock::now();
  bool done = false;

  while (!done) {
    // Poll the bus for messages with a timeout of 1 second (in nanoseconds)
    GstMessage* msg = gst_bus_timed_pop(bus, 1000000000);
    if (msg) {
      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
          std::cout << "End-of-stream reached." << std::endl;
          done = true;
          break;
        case GST_MESSAGE_ERROR: {
          GError* err = nullptr;
          gchar* debug = nullptr;
          gst_message_parse_error(msg, &err, &debug);
          std::cerr << "Error from element " << GST_OBJECT_NAME(msg->src) << ": " << err->message << std::endl;
          if (debug)
            std::cerr << "Debug info: " << debug << std::endl;
          g_error_free(err);
          g_free(debug);
          done = true;
          break;
        }
        default:
          break;
      }
      gst_message_unref(msg);
    }

    // Check the current state of the pipeline without blocking
    GstState currentState, pendingState;
    gst_element_get_state(pipeline, &currentState, &pendingState, 0);
    if (currentState != GST_STATE_PLAYING) {
      done = true;
    }

    // Every 5 seconds, print a waiting message
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPrintTime).count() >= 5) {
      std::cout << "Waiting for pipeline to stop..." << std::endl;
      lastPrintTime = now;
    }
  }

  gst_object_unref(bus);
}

// Returns a new reference to the pipeline element or nullptr if not found.
// Caller is responsible for unref'ing the returned pipeline.
GstReferencedObject<GstElement*> get_pipeline_element(GstElement* element) {
  if (!element)
    return nullptr;

  // Get parent of the current element (this returns a new reference)
  GstReferencedObject<GstElement*> parent = GST_ELEMENT(gst_element_get_parent(element));
  if (!parent)
    return nullptr;

  if (GST_IS_PIPELINE(parent.get())) {
    return parent; // Found the pipeline; caller must unref it.
  }

  // Otherwise, recursively search the parent's parent.
  GstReferencedObject<GstElement*> pipeline = get_pipeline_element(parent);
  return pipeline;
}

} // namespace hm
