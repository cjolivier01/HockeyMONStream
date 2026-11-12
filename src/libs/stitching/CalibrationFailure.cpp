#include "hstream/src/libs/stitching/CalibrationFailure.h"
#include "hstream/src/libs/stitching/StitchingAlgorithms.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace hm::stitching::failure {
namespace {
bool contains(const std::string& value, const std::string& part) {
  return value.find(part) != std::string::npos;
}
bool starts(const std::string& value, const std::string& prefix) {
  return value.compare(0, prefix.size(), prefix) == 0;
}
std::string trim(std::string value) {
  value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
  const auto first = value.find_first_not_of(" \t\n");
  return first == std::string::npos ? std::string() : value.substr(first, value.find_last_not_of(" \t\n") - first + 1);
}
std::string lower(std::string value) {
  for (char& c : value)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return value;
}
std::string detail(const std::string& value) {
  std::size_t end = std::string::npos;
  for (const char* code :
       {"CANCELLED:",
        "UNKNOWN:",
        "INVALID_ARGUMENT:",
        "DEADLINE_EXCEEDED:",
        "NOT_FOUND:",
        "ALREADY_EXISTS:",
        "PERMISSION_DENIED:",
        "RESOURCE_EXHAUSTED:",
        "FAILED_PRECONDITION:",
        "ABORTED:",
        "OUT_OF_RANGE:",
        "UNIMPLEMENTED:",
        "INTERNAL:",
        "UNAVAILABLE:",
        "DATA_LOSS:",
        "UNAUTHENTICATED:"}) {
    for (std::size_t position = value.find(code); position != std::string::npos;
         position = value.find(code, position + 1)) {
      if (position && (std::isalnum(static_cast<unsigned char>(value[position - 1])) || value[position - 1] == '_'))
        continue;
      const auto next = position + std::char_traits<char>::length(code);
      if (end == std::string::npos || next > end)
        end = next;
    }
  }
  return end == std::string::npos ? std::string() : trim(value.substr(end));
}
bool specific(const std::string& value) {
  const auto normalized = lower(value);
  return !normalized.empty() && normalized != "app run failed" &&
      normalized != "pipeline failed during stitching calibration" &&
      !starts(normalized, "no stitching calibration frame pair");
}
std::string number(double value) {
  std::ostringstream out;
  out << std::setprecision(6) << value;
  return out.str();
}
std::string width_guidance(int width) {
  // Existing UI advice floor, not a processing/configuration default.
  constexpr int floor = 4096;
  if (width <= 0)
    return "Max stitched width was Auto, so HStream does not have a numeric stitched-width limit to halve. "
           "Set Max stitched width to 4096 pixels and press Play to retry; 4096 is the lowest reasonable value.";
  const std::string used = "This run used Max stitched width " + std::to_string(width) + " pixels";
  if (width <= floor)
    return used + ", which is already " + (width == floor ? "at" : "below") +
        " the 4096-pixel lowest reasonable value; do not reduce it further.";
  const int half = width / 2, suggested = std::max(floor, half);
  const std::string suggestion = std::to_string(suggested) +
      (suggested == half ? std::string(" pixels (half") + (width % 2 ? ", rounded down)" : ")")
                         : " pixels (the lowest reasonable value)");
  return used + ". Set it below " + std::to_string(width) + " and press Play to retry; try " + suggestion + ". " +
      (suggested == floor ? "4096 is the lowest reasonable value; do not reduce it further."
                          : "If another stitching OOM occurs, halve it again, stopping at 4096 pixels.");
}
std::string matcher_label(const std::string& value) {
  if (value == "superpoint-lightglue")
    return "SuperPoint + LightGlue";
  if (value == "dedode-lightglue")
    return "DeDoDe + LightGlue";
  if (value == "loftr")
    return "LoFTR (EfficientLoFTR outdoor)";
  if (value == "akaze-hamming")
    return "AKAZE + M-LDB + Hamming";
  return value.empty() ? "unknown matcher" : value;
}
std::string backend_label(const std::string& value) {
  if (value == "nona")
    return "NONA";
  if (value == "opencv-magsac")
    return "MAGSAC++";
  if (value == "opencv-affine-ransac")
    return "RANSAC";
  return value.empty() ? "unknown mapping backend" : value;
}
std::string projection_label(const std::string& value) {
  for (const auto& info : SupportedStitchProjections())
    if (value == info.name)
      return info.display_name;
  return value.empty() ? "unknown projection" : value;
}
} // namespace

bool IsCudaOutOfMemory(const std::string& line) {
  const auto value = lower(line);
  bool status_two = false;
  for (std::size_t position = value.find("cuda failure:"); position != std::string::npos;
       position = value.find("cuda failure:", position + 1)) {
    auto start = position + 13;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
      ++start;
    const auto end = start + 8;
    if (value.compare(start, 8, "status=2") == 0 &&
        (end == value.size() || !std::isdigit(static_cast<unsigned char>(value[end]))))
      status_two = true;
  }
  return status_two || contains(value, "cudaerrormemoryallocation") || contains(value, "cuda_error_out_of_memory") ||
      (starts(value, "cuda error at ") && contains(value, ": out of memory"));
}
bool IsStallWatchdog(const std::string& line) {
  return contains(lower(line), "no-progress watchdog expired");
}
std::string StageLabel(const std::string& stage) {
  static const std::map<std::string, std::string> labels{
      {"input", "Wait for synchronized camera frames"},
      {"orientation", "Find the ice rink and orient cameras"},
      {"features", "Look for control points"},
      {"matching", "Match control points"},
      {"optimizer", "Run panorama optimizer (autooptimiser)"},
      {"leveling", "Optionally level the rink"},
      {"canvas", "Build stitch maps and panorama"},
      {"rink-mask", "Find the ice surface"},
      {"playback-restart", "Restarting playback"}};
  const auto it = labels.find(stage);
  return it == labels.end() ? (stage.empty() ? "calibration" : stage) : it->second;
}
bool Evidence::CudaOutOfMemory() const {
  for (const auto& item : allocations_)
    if (item.second.oom)
      return true;
  return false;
}
bool Evidence::BlenderOutOfMemory() const {
  for (const auto& item : allocations_)
    if (item.second.blender)
      return true;
  return false;
}
bool Evidence::InputPoolFailure() const {
  for (const auto& item : allocations_)
    if (item.second.input_pool && item.second.oom)
      return true;
  return false;
}
void Evidence::Record(const std::string& line, std::uint64_t generation, bool context_bound) {
  const auto diagnostic = trim(line);
  if (diagnostic.empty())
    return;
  latest_generation_ = generation;
  latest_context_bound_ = context_bound;
  if (starts(diagnostic, "HSTREAM_ONNX_FALLBACK provider=cpu reason=cuda-out-of-memory ")) {
    allocations_.clear();
    recent_.clear();
    return;
  }
  constexpr char prefix[] = "HSTREAM_CALIBRATION stage=";
  if (starts(diagnostic, prefix)) {
    const auto separator = diagnostic.find(" status=", sizeof(prefix) - 1);
    if (separator != std::string::npos) {
      const auto stage = diagnostic.substr(sizeof(prefix) - 1, separator - (sizeof(prefix) - 1));
      const auto status_start = separator + 8;
      const auto message = diagnostic.find(" message=", status_start);
      const auto status =
          diagnostic.substr(status_start, message == std::string::npos ? std::string::npos : message - status_start);
      const bool known =
          !stage.empty() && (stage == "playback-restart" || stage == "calibration" || StageLabel(stage) != stage);
      if (known && (status == "started" || status == "complete" || status == "failed" || status == "skipped")) {
        if (stage == "playback-restart") {
          if (status == "skipped")
            return;
          restart_ = status == "started" ? "pending" : status;
          if (status == "started" || status == "failed")
            stage_ = stage;
        } else if (stage == "calibration") {
          solve_ = status;
          if (status == "complete" && restart_ == "not-observed")
            restart_ = "pending";
        } else if (status == "started" || status == "failed") {
          stage_ = stage;
          solve_ = status;
        }
        if (status == "failed")
          terminal_ = {
              message == std::string::npos ? std::string() : diagnostic.substr(message + 9), generation, context_bound};
      }
    }
  }
  const auto normalized = lower(diagnostic);
  const bool oom = IsCudaOutOfMemory(diagnostic);
  const bool pool = contains(normalized, "hmstitcher_conv0") && contains(normalized, "failed to activate bufferpool");
  if (oom || pool) {
    auto& allocation = allocations_[{generation, context_bound}];
    allocation.oom |= oom;
    allocation.input_pool |= pool;
    allocation.blender |= oom && contains(normalized, "cudablend");
  }
  const bool watchdog = IsStallWatchdog(diagnostic);
  if (watchdog && watchdog_.text.empty())
    watchdog_ = {diagnostic, generation, context_bound};
  const bool hypothesis = starts(diagnostic, "Rejected calibrated MAGSAC hypothesis");
  const bool candidate = starts(diagnostic, "Skipping pooled stitching calibration") ||
      starts(diagnostic, "Skipping stitching calibration frame pair");
  const bool buffer = contains(normalized, "gst_nvds_buffer_pool_alloc_buffer") ||
      contains(normalized, "failed to activate bufferpool") || contains(normalized, "error(-1) in buffer allocation");
  if (!(oom || pool || buffer || hypothesis || candidate || watchdog || starts(diagnostic, "Trying ") ||
        starts(diagnostic, "OpenCV mapping backend ") || !detail(diagnostic).empty() ||
        contains(diagnostic, "HSTREAM_CALIBRATION") || contains(normalized, "enblend") ||
        contains(normalized, "autooptimiser")))
    return;
  if (hypothesis && rejected_hypotheses_ < 1000000)
    ++rejected_hypotheses_;
  if (candidate && rejected_candidates_ < 1000000)
    ++rejected_candidates_;
  const Line value{diagnostic, generation, context_bound};
  if (recent_.empty() || recent_.back().text != value.text || recent_.back().generation != generation ||
      recent_.back().context_bound != context_bound)
    recent_.push_back(value);
  if (recent_.size() > 24)
    recent_.erase(recent_.begin());
}

Diagnosis Evidence::Analyze(
    const std::string& terminal,
    const std::map<std::uint64_t, Context>& contexts,
    const Context* fallback,
    const std::string& stage_override) const {
  Diagnosis result;
  result.solve_outcome = solve_;
  result.restart_outcome = restart_;
  result.reported_cause = trim(terminal);
  result.cause_generation = latest_generation_;
  bool context_bound = latest_context_bound_;
  if (!specific(detail(result.reported_cause)) && !terminal_.text.empty()) {
    result.reported_cause = terminal_.text;
    result.cause_generation = terminal_.generation;
    context_bound = terminal_.context_bound;
  }
  auto allocations = allocations_;
  if (IsCudaOutOfMemory(result.reported_cause)) {
    auto& item = allocations[{result.cause_generation, context_bound}];
    item.oom = true;
    item.blender |= contains(lower(result.reported_cause), "cudablend");
    item.input_pool |= contains(lower(result.reported_cause), "hmstitcher_conv0") &&
        contains(lower(result.reported_cause), "failed to activate bufferpool");
  }
  bool oom = false, pool = false, blender = false;
  for (int priority = 0; priority < 3 && !oom; ++priority) {
    for (const auto& [origin, item] : allocations) {
      if (!item.oom || (priority == 0 && !item.blender) || (priority == 1 && !item.input_pool))
        continue;
      oom = true;
      pool = item.input_pool;
      blender = item.blender;
      result.cause_generation = origin.first;
      context_bound = origin.second;
      break;
    }
  }
  const bool watchdog = !watchdog_.text.empty() || IsStallWatchdog(result.reported_cause);
  if (oom) {
    result.reported_cause = blender ? "CUDA out of memory while allocating the live stitching blender's GPU buffers."
        : pool
        ? "CUDA out of memory (cudaErrorMemoryAllocation, status 2) while activating hmstitcher's pre-stitch input-conversion buffer pool."
        : "CUDA out of memory (cudaErrorMemoryAllocation, status 2) while allocating a calibration GPU buffer.";
  } else if (!watchdog_.text.empty()) {
    result.reported_cause = watchdog_.text;
    result.cause_generation = watchdog_.generation;
    context_bound = watchdog_.context_bound;
  } else if (!specific(detail(result.reported_cause))) {
    std::optional<Line> selected;
    for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
      if (starts(it->text, "HSTREAM_CALIBRATION"))
        continue;
      const auto status = detail(it->text);
      if (status.empty())
        continue;
      if (!selected)
        selected = *it;
      if (specific(status)) {
        selected = *it;
        break;
      }
    }
    if (selected) {
      result.reported_cause = selected->text;
      result.cause_generation = selected->generation;
      context_bound = selected->context_bound;
    }
  }
  if (result.reported_cause.empty())
    result.reported_cause = "The calibration process ended without reporting a specific cause.";
  // Context emitted after a cause cannot retroactively authorize settings advice.
  // Keep its real scope internally so unbound allocations from different passes
  // never combine; generation zero in the result means no context was used.
  if (!context_bound)
    result.cause_generation = 0;
  const auto found = contexts.find(result.cause_generation);
  const Context* context = found == contexts.end() ? fallback : &found->second;
  const auto evidence = lower(result.reported_cause);
  const bool geometry = contains(evidence, "no stitching calibration frame pair") ||
      contains(evidence, "usable solution") || contains(evidence, "usable hugin solution") ||
      contains(evidence, "usable opencv solution");
  const bool missing_hugin = contains(evidence, "required hugin executable not found") ||
      contains(evidence, "hm_pto_gen is not executable") || contains(evidence, "hm_autooptimiser is not executable") ||
      contains(evidence, "hm_pano_modify is not executable") || contains(evidence, "hm_nona is not executable") ||
      contains(evidence, "hm_enblend is not executable");
  result.category = "terminal";
  if (blender) {
    result.category = "cuda-oom-blender";
    result.explanation =
        "The GPU ran out of VRAM during live video blending after the stitch maps were loaded. CPU fallback covers control-point and ice-detection models; live video blending still requires GPU memory.";
    result.action =
        "Close other GPU-intensive applications." + (context ? " " + width_guidance(context->max_output_width) : "");
  } else if (oom) {
    result.category = pool ? "cuda-oom-input-pool" : "cuda-oom";
    result.explanation = pool
        ? "The GPU ran out of VRAM while preparing the native-resolution camera frames for stitching. This happened before frame matching, NONA, or optional rink leveling began."
        : "The GPU ran out of VRAM while calibration was allocating a required GPU buffer.";
    result.action = "Close other GPU-intensive applications and press Play to retry. ";
    if (pool) {
      result.action +=
          "If VRAM is still insufficient, set 10-bit / FP16 mode to Force off or use lower-resolution source video.";
      if (context)
        result.action += " " + width_guidance(context->max_output_width) +
            " This setting reduces later stitched-canvas memory; it does not reduce the pre-stitch native-resolution allocation that failed here.";
    } else {
      if (context)
        result.action += width_guidance(context->max_output_width) + " ";
      result.action +=
          "If the failure occurred before canvas generation, also set 10-bit / FP16 mode to Force off or use lower-resolution source video.";
    }
  } else if (watchdog) {
    result.category = "watchdog";
    result.explanation =
        "A timeout ended this run because neither camera stream made progress within the watchdog budget. Calibration work and operator review suspend this watchdog. This message alone cannot identify which part of the pipeline stopped advancing.";
    result.action =
        "Press Play to retry. If it keeps happening at the same point, start HStream with HM_URI_PLAYLIST_BARRIER_STALL_SECONDS set to a larger number of seconds, or to 0 to disable the watchdog, and report the run: a stall during calibration is a bug in the watchdog's suspension, not a setting you should have to change.";
  } else if (missing_hugin) {
    result.category = "hugin-toolchain";
    result.explanation = "Calibration found control points, but the Hugin/NONA command-line toolchain is incomplete.";
    result.action =
        "Install the Hugin command-line tools or set HM_PTO_GEN, HM_AUTOOPTIMISER, HM_PANO_MODIFY, HM_NONA, and HM_ENBLEND to valid executables, then press Play to retry.";
  } else if (
      contains(evidence, "enblend") || contains(evidence, "seam_file") || contains(evidence, "seam file") ||
      contains(evidence, "artifact publication") || contains(evidence, "publish stitch artifact")) {
    result.category = "seam-publication";
    result.explanation = "Alignment completed, but the seam/panorama generation or artifact publication step failed.";
    result.action = "Check the reported diagnostic, available disk space, and write access to the game directory.";
  } else if (contains(evidence, "autooptimiser")) {
    result.category = "optimizer";
    result.explanation = "Hugin's panorama optimizer rejected the selected control-point geometry.";
    result.action =
        "Try a frame with clearer overlap or use AKAZE + MAGSAC++ + Rectilinear, which does not depend on Hugin autooptimization.";
  } else if (
      contains(evidence, "unsafe") || contains(evidence, "pole") || contains(evidence, "canvas extent") ||
      contains(evidence, "canvas dimension") || contains(evidence, "output surface is smaller")) {
    result.category = "unsafe-geometry";
    result.explanation =
        "The fitted transform projected the cameras to an unsafe or implausibly large canvas. HStream rejected it instead of allocating that canvas.";
    result.action =
        "Choose a frame with more unique overlap detail or increase Frames. Also verify the Mission lens profile; Max stitched width can limit memory, but it cannot repair an incorrect transform.";
  } else if (
      contains(evidence, "left_calibration.json") || contains(evidence, "lens profile") || contains(evidence, "kb4") ||
      contains(evidence, "calibrated akaze")) {
    result.category = "lens-profile";
    result.explanation = "The calibrated AKAZE path could not load or consistently apply both camera lens profiles.";
    result.action =
        "Verify left_calibration.json is a regular, readable Mission KB4 profile for these recordings and that AKAZE is paired with MAGSAC++ and Rectilinear.";
  } else if (
      contains(evidence, "resource_exhausted") || contains(evidence, "allocate") ||
      contains(evidence, "out of memory") || (contains(evidence, "cuda") && contains(evidence, "memory"))) {
    result.category = "resource-exhausted";
    result.explanation = "Calibration exhausted CPU/GPU memory while building the mapping canvas.";
    result.action = "Set a lower Max stitched width, close other GPU workloads, and press Play to retry.";
  } else if (
      geometry && context &&
      (context->control_point_limit < context->default_control_point_limit || context->camera_differs_from_defaults)) {
    result.category = "selected-geometry";
    std::string settings;
    if (context->control_point_limit < context->default_control_point_limit)
      settings = "CP limit is " + std::to_string(context->control_point_limit) + "; the default calibration limit is " +
          std::to_string(context->default_control_point_limit);
    if (context->camera_differs_from_defaults) {
      if (!settings.empty())
        settings += "; ";
      settings += "lens profile is " + context->camera.configuration + " (HFOV " +
          number(context->camera.horizontal_fov) + ", VFOV " + number(context->camera.vertical_fov) + "); default is " +
          context->default_camera.configuration + " (HFOV " + number(context->default_camera.horizontal_fov) +
          ", VFOV " + number(context->default_camera.vertical_fov) + ")";
    }
    result.explanation =
        "Calibration could not produce a safe geometry with the selected calibration settings. This does not by itself prove that the camera views lack overlap.";
    result.action = "Restore the known-good stitching settings and retry: " + settings +
        ". If it still fails, choose a frame with more unique overlap detail or increase Frames.";
  } else if (
      contains(evidence, "insufficient consensus") || contains(evidence, "usable match") ||
      contains(evidence, "control point") || contains(evidence, "control-point") ||
      contains(evidence, "no stitching calibration frame pair")) {
    result.category = "overlap";
    result.explanation =
        "The cameras did not produce enough geometrically consistent overlap. Repeated rink markings can create many descriptor matches while still leaving too few matches for one safe transform.";
    result.action =
        "Choose a stitch frame with players, boards, or other unique detail in the overlap; increase Frames; verify the Left/Right assignments and lens profile; then press Play to retry.";
  } else {
    result.explanation = "The active calibration stage returned a terminal error.";
    result.action =
        "Review the diagnostic lines below, correct the reported input/configuration problem, and press Play to retry.";
  }
  const auto stage = !stage_override.empty() ? stage_override
      : !stage_.empty()                      ? stage_
      : context                              ? context->calibration_start_stage
                                             : std::string();
  result.stage = blender ? "Live stitching blend" : StageLabel(stage);
  if (context)
    result.configuration = matcher_label(context->matcher) + " · " + backend_label(context->mapping_backend) + " · " +
        projection_label(context->projection) + " · " + std::to_string(context->frame_count) + " sampled frame" +
        (context->frame_count == 1 ? "" : "s");
  else
    result.configuration = "Native launch context unavailable";
  result.applicability = "none";
  if (restart_ == "failed")
    result.applicability = "playback-restart";
  else if (solve_ == "failed" || solve_ == "started")
    result.applicability = "calibration";
  else if (solve_ == "not-observed" && context && context->calibration_required)
    result.applicability = "calibration";
  else if ((solve_ == "complete" || solve_ == "skipped") && restart_ != "complete" && blender)
    result.applicability = "playback-restart";
  else if (watchdog && solve_ == "not-observed")
    result.applicability = "runtime";
  result.rejected_hypotheses = rejected_hypotheses_;
  result.rejected_candidates = rejected_candidates_;
  result.reported_cause = result.reported_cause.substr(0, 4096);
  result.rendered_text = "Failed stage: " + result.stage + "\nConfiguration: " + result.configuration +
      "\n\nWhy it failed\n" + result.explanation + "\n\nReported cause\n" + result.reported_cause +
      "\n\nWhat to try\n" + result.action;
  if (rejected_hypotheses_ || rejected_candidates_)
    result.rendered_text += "\n\nBounded fallback search\nHStream rejected " + std::to_string(rejected_hypotheses_) +
        " projective hypothes" + (rejected_hypotheses_ == 1 ? "is" : "es") + " and " +
        std::to_string(rejected_candidates_) + " frame-set candidate" + (rejected_candidates_ == 1 ? "" : "s") +
        " before stopping. These rejections are expected safety fallbacks, not automatic restarts; pressing Play is required for a new calibration run.";
  for (auto it = recent_.rbegin(); it != recent_.rend() && result.evidence.size() < 6; ++it)
    if (!starts(it->text, "HSTREAM_CALIBRATION"))
      result.evidence.insert(result.evidence.begin(), it->text.substr(0, 4096));
  if (!result.evidence.empty()) {
    result.rendered_text += "\n\nRecent diagnostics";
    for (const auto& line : result.evidence)
      result.rendered_text += "\n• " + line;
  }
  result.rendered_text += "\n\nThe runtime log contains the complete output.";
  return result;
}
} // namespace hm::stitching::failure
