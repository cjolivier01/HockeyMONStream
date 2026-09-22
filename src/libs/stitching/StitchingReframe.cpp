#include "hstream/src/libs/stitching/StitchingReframe.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;
constexpr size_t kMaximumIntentBytes = 256 * 1024;

YAML::Node node_at(const YAML::Node& node, std::initializer_list<const char*> path) {
  YAML::Node value = node;
  for (const char* key : path) {
    if (!value || !value.IsMap())
      return YAML::Node(YAML::NodeType::Undefined);
    const YAML::Node child = static_cast<const YAML::Node&>(value)[key];
    if (!child.IsDefined())
      return YAML::Node(YAML::NodeType::Undefined);
    value.reset(child);
  }
  return value;
}

YAML::Node calibration(const YAML::Node& config) {
  return node_at(config, {"hstream_ui", "stitching_calibration"});
}

std::string text_or(const YAML::Node& value, const std::string& fallback = {}) {
  return value && !value.IsNull() ? value.as<std::string>() : fallback;
}

void overlay(YAML::Node& destination, const YAML::Node& source) {
  if (!source || source.IsNull())
    return;
  for (const auto& entry : source) {
    const auto key = entry.first.as<std::string>();
    if (entry.second.IsMap()) {
      if (!destination[key] || !destination[key].IsMap())
        destination[key] = YAML::Node(YAML::NodeType::Map);
      YAML::Node child = destination[key];
      overlay(child, entry.second);
    } else {
      destination[key] = YAML::Clone(entry.second);
    }
  }
}

absl::StatusOr<YAML::Node> effective_config(const YAML::Node& config) {
  if (!config || !config.IsMap())
    return absl::InvalidArgumentError("Reframe configuration must be a map");
  const auto baseline = hm::baseline_config::load();
  if (!baseline.ok())
    return baseline.status();
  YAML::Node effective = YAML::Clone(baseline->values);
  const auto user_path = hm::user_config::file_path();
  if (user_path.ok()) {
    std::error_code error;
    const bool exists = fs::exists(*user_path, error);
    if (error)
      return absl::InternalError("Cannot inspect user configuration for reframe: " + error.message());
    if (exists) {
      const YAML::Node user = YAML::LoadFile(user_path->string());
      if (user && !user.IsNull() && !user.IsMap())
        return absl::InvalidArgumentError("User configuration must be a map");
      overlay(effective, user);
    }
  }
  overlay(effective, config);
  return effective;
}

// Stable ordering also makes absent/null optional source data compare equally.
YAML::Node canonical(const YAML::Node& node, size_t depth = 0) {
  if (depth > 24)
    throw YAML::RepresentationException(YAML::Mark::null_mark(), "Reframe data is too deeply nested");
  if (!node || node.IsNull())
    return YAML::Node(YAML::NodeType::Null);
  if (node.IsMap()) {
    std::map<std::string, YAML::Node> sorted;
    for (const auto& entry : node)
      sorted.emplace(entry.first.as<std::string>(), entry.second);
    YAML::Node result(YAML::NodeType::Map);
    for (const auto& entry : sorted)
      result[entry.first] = canonical(entry.second, depth + 1);
    return result;
  }
  if (node.IsSequence()) {
    YAML::Node result(YAML::NodeType::Sequence);
    for (const auto& entry : node)
      result.push_back(canonical(entry, depth + 1));
    return result;
  }
  return YAML::Clone(node);
}

std::string canonical_text(const YAML::Node& node) {
  return YAML::Dump(canonical(node));
}

absl::StatusOr<size_t> nonnegative_size(const YAML::Node& node, const char* label, size_t fallback = 0) {
  if (!node || node.IsNull())
    return fallback;
  if (!node.IsScalar())
    return absl::InvalidArgumentError(std::string(label) + " must be a nonnegative integer");
  const auto value = node.as<std::string>();
  size_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
    return absl::InvalidArgumentError(std::string(label) + " must be a nonnegative integer");
  return result;
}

struct Snapshot {
  StitchingBackendChoices choices;
  YAML::Node solver;
  YAML::Node view;
  size_t max_output_width{0};
  size_t max_output_dimension{0};
};

absl::StatusOr<Snapshot> snapshot(const YAML::Node& config, bool require_completed_inputs = true) {
  Snapshot result;
  auto& choices = result.choices;
  const YAML::Node stitching = node_at(config, {"stitching"});
  const YAML::Node state = calibration(config);
  ControlPointMatcher matcher;
  MappingBackend backend;
  StitchProjection projection;
  HM_ASSIGN_OR_RETURN(matcher, ParseControlPointMatcher(text_or(stitching["control_point_matcher"])));
  HM_ASSIGN_OR_RETURN(backend, ParseMappingBackend(text_or(stitching["mapping_backend"])));
  HM_ASSIGN_OR_RETURN(projection, ParseStitchProjection(text_or(stitching["projection"])));
  choices.control_point_matcher = ControlPointMatcherName(matcher);
  choices.mapping_backend = MappingBackendName(backend);
  choices.projection = StitchProjectionName(projection);
  choices.run_autooptimizer = stitching["run_autooptimizer"].as<bool>();
  HM_ASSIGN_OR_RETURN(choices.projection_parameters, read_stitch_projection_parameters(config, projection));
  HM_ASSIGN_OR_RETURN(choices.projection_framing, read_stitch_projection_framing(config));
  HM_ASSIGN_OR_RETURN(choices.camera, read_stitch_camera_selection(config));
  HM_ASSIGN_OR_RETURN(choices.control_point_resolution, read_control_point_resolution(config));
  HM_ASSIGN_OR_RETURN(choices.control_point_execution_provider, read_control_point_execution_provider(config));
  HM_ASSIGN_OR_RETURN(choices.calibration_frame_selection_fingerprint, player_frame_selection_fingerprint(config));
  HM_RETURN_IF_ERROR(ValidateMappingBackendProjection(backend, projection));
  if (backend == MappingBackend::kNona)
    HM_RETURN_IF_ERROR(
        ValidateStitchProjectionFraming(projection, choices.projection_parameters, choices.projection_framing));
  size_t points = 0;
  size_t count = 0;
  HM_ASSIGN_OR_RETURN(points, nonnegative_size(node_at(state, {"control_points"}), "control_points"));
  HM_ASSIGN_OR_RETURN(count, nonnegative_size(node_at(state, {"frame_count"}), "frame_count"));
  if (require_completed_inputs && (!points || !count || count > kPlayerFrameMaximumPairs))
    return absl::FailedPreconditionError("Reframe requires the saved positive control-point budget and frame count");
  // The completed UI/worker record is authoritative. Older runs may have
  // supplied this count through an option without materializing the canonical
  // key, so a newly inherited baseline count cannot redefine the old solve.
  if (require_completed_inputs && !choices.calibration_frame_selection_fingerprint.empty() &&
      node_at(stitching, {"calibration_frame_selection", "selected"}).size() != count)
    return absl::FailedPreconditionError("Reframe selected plan conflicts with the saved calibration frame count");
  auto& solver = result.solver;
  solver["control_points"] = points;
  solver["frame_count"] = count;
  solver["control_point_matcher"] = choices.control_point_matcher;
  solver["mapping_backend"] = choices.mapping_backend;
  solver["run_autooptimizer"] = choices.run_autooptimizer;
  solver["control_point_resolution"] = ControlPointResolutionName(choices.control_point_resolution);
  solver["control_point_execution_provider"] =
      hm::onnx::ExecutionProviderName(choices.control_point_execution_provider);
  solver["camera_config"] = choices.camera.configuration;
  solver["camera_fov"]["horizontal_fov"] = choices.camera.horizontal_fov;
  solver["camera_fov"]["vertical_fov"] = choices.camera.vertical_fov;
  solver["calibration_frame_selection_fingerprint"] = choices.calibration_frame_selection_fingerprint;
  solver["calibration_frame_inputs_fingerprint"] = text_or(stitching["calibration_frame_inputs_fingerprint"]);
  solver["stitch_frame_time"] = text_or(stitching["stitch_frame_time"], "00:00:00");
  solver["videos"] = canonical(node_at(config, {"game", "videos"}));
  YAML::Node offsets = node_at(config, {"game", "stitching", "frame_offsets"});
  if (!offsets || offsets.IsNull())
    offsets.reset(stitching["frame_offsets"]);
  solver["frame_offsets"] = canonical(offsets);
  solver["offsets"] = canonical(stitching["offsets"]);
  solver["source_projection"] = canonical(node_at(config, {"video_in", "projection"}));
  solver["source_rotation"] = canonical(node_at(config, {"video_in", "rotate_degrees"}));
  auto& view = result.view;
  view["projection"] = choices.projection;
  view["projection_parameters"] = choices.projection_parameters;
  YAML::Node framing;
  write_stitch_projection_framing(framing, choices.projection_framing);
  // Freeze the resolved angles even when the effective value inherited a rink.
  framing["stitching"]["projection_framing"]["rotation_degrees"] = choices.projection_framing.rotation_degrees;
  view["projection_framing"] = framing["stitching"]["projection_framing"];
  HM_ASSIGN_OR_RETURN(result.max_output_width, nonnegative_size(stitching["max_output_width"], "max_output_width"));
  // The native Hugin worker derives this limit from platform/environment,
  // rather than the historical canonical max_output_dimension setting.
  result.max_output_dimension = live_stitch_max_canvas_dimension().value_or(0);
  view["max_output_width"] = result.max_output_width;
  view["max_output_dimension"] = result.max_output_dimension;
  return result;
}

absl::Status supported(const Snapshot& requested) {
  if (requested.choices.mapping_backend != "nona" || !requested.choices.run_autooptimizer)
    return absl::FailedPreconditionError(
        "Reframing requires an optimized NONA alignment; explicitly recalibrate to change the solver");
  if (!requested.choices.projection_framing.auto_canvas)
    return absl::FailedPreconditionError("Reframing this saved alignment requires projection_framing.auto_canvas=true");
  return absl::OkStatus();
}

absl::StatusOr<std::string> file_sha256(const fs::path& path, size_t maximum_bytes, bool follow_symlink = false) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (follow_symlink ? 0 : O_NOFOLLOW));
  if (descriptor < 0)
    return absl::FailedPreconditionError("Cannot read reframe source " + path.string() + ": " + std::strerror(errno));
  struct Descriptor {
    int value;
    ~Descriptor() {
      ::close(value);
    }
  } cleanup{descriptor};
  struct stat before{};
  if (::fstat(descriptor, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
      static_cast<uint64_t>(before.st_size) > maximum_bytes)
    return absl::FailedPreconditionError("Reframe source must be a nonempty bounded regular file: " + path.string());
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    return absl::InternalError("Cannot initialize reframe source hash");
  std::array<unsigned char, 64 * 1024> buffer{};
  uint64_t total = 0;
  for (;;) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      return absl::InternalError("Cannot read reframe source " + path.string());
    if (!count)
      break;
    total += static_cast<size_t>(count);
    if (total > maximum_bytes || EVP_DigestUpdate(context.get(), buffer.data(), count) != 1)
      return absl::AbortedError("Reframe source changed during hashing");
  }
  struct stat after{};
  if (::fstat(descriptor, &after) || total != static_cast<uint64_t>(before.st_size) ||
      before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    return absl::AbortedError("Reframe source changed during hashing");
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(context.get(), bytes.data(), &size) != 1 || size != 32)
    return absl::InternalError("Cannot finalize reframe source hash");
  std::ostringstream encoded;
  for (size_t index = 0; index < size; ++index)
    encoded << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(bytes[index]);
  return encoded.str();
}

absl::Status validate_provenance(const HuginProject::CanvasProvenance& source, const Snapshot& original) {
  const auto& choices = original.choices;
  if (!source.mapping_backend || *source.mapping_backend != MappingBackend::kNona || !source.projection ||
      StitchProjectionName(*source.projection) != choices.projection || !source.projection_parameters ||
      *source.projection_parameters != choices.projection_parameters || !source.projection_framing ||
      *source.projection_framing != choices.projection_framing || !source.camera || *source.camera != choices.camera ||
      !source.control_point_matcher ||
      ControlPointMatcherName(*source.control_point_matcher) != choices.control_point_matcher ||
      !source.control_point_resolution || *source.control_point_resolution != choices.control_point_resolution ||
      source.calibration_frame_selection_fingerprint != choices.calibration_frame_selection_fingerprint)
    return absl::FailedPreconditionError(
        "Published alignment provenance does not match the original completed calibration");
  return absl::OkStatus();
}

absl::Status validate_selected_inputs(
    const fs::path& directory,
    const YAML::Node& config,
    const HuginProject::CanvasProvenance& source,
    const std::array<std::string, 2>& hashes) {
  if (source.calibration_frame_selection_fingerprint.empty())
    return absl::OkStatus();
  PlayerFrameSelectionPlan plan;
  HM_ASSIGN_OR_RETURN(
      plan, ParsePlayerFrameSelectionPlan(node_at(config, {"stitching", "calibration_frame_selection"})));
  if (plan.fingerprint != source.calibration_frame_selection_fingerprint)
    return absl::FailedPreconditionError("Reframe selected input identity changed");
  const auto materialized = text_or(node_at(config, {"stitching", "calibration_frame_inputs_fingerprint"}));
  if (!materialized.empty() && materialized != plan.fingerprint)
    return absl::FailedPreconditionError("Reframe retained input reference does not match the selected plan");
  const auto inputs = LoadPlayerFrameInputs(directory, plan);
  if (!inputs.ok())
    return inputs.status();
  if (!inputs->has_value())
    return absl::FailedPreconditionError("Reframe requires the complete retained selected PNG bundle");
  const std::string prefix = "representative=";
  const auto& diagnostics = source.calibration_frame_diagnostics;
  const auto separator = diagnostics.find(';');
  if (diagnostics.rfind(prefix, 0) != 0 || separator == std::string::npos)
    return absl::FailedPreconditionError("Published selected alignment has no representative frame identity");
  size_t index = 0;
  const auto parsed = std::from_chars(diagnostics.data() + prefix.size(), diagnostics.data() + separator, index);
  if (parsed.ec != std::errc() || parsed.ptr != diagnostics.data() + separator ||
      index >= (*inputs)->image_digests.size() || (*inputs)->image_digests[index] != hashes)
    return absl::FailedPreconditionError("Published representative PNGs do not match their retained selected pair");
  return absl::OkStatus();
}

absl::StatusOr<HuginProject::CanvasProvenance> source_provenance(const fs::path& directory) {
  std::string contents;
  HM_ASSIGN_OR_RETURN(
      contents,
      read_bounded_regular_file_no_follow(
          directory / kStitchCanvasProvenanceArtifact, 4096, "reframe canvas provenance"));
  if (contents.rfind("version=10\n", 0) != 0)
    return absl::FailedPreconditionError("Reframing requires complete version-10 alignment provenance");
  const auto provenance = HuginProject::ReadCanvasProvenanceLocked(directory);
  if (!provenance.ok())
    return provenance.status();
  if (!provenance->has_value())
    return absl::FailedPreconditionError("Reframe source provenance is missing");
  return **provenance;
}

absl::Status validate_akaze_profile(const fs::path& directory, const HuginProject::CanvasProvenance& source) {
  if (!source.control_point_matcher || *source.control_point_matcher != ControlPointMatcher::kAkazeHamming)
    return absl::OkStatus();
  std::error_code error;
  const bool exists = fs::exists(directory / "left_calibration.json", error);
  if (error)
    return absl::InternalError("Cannot inspect saved lens calibration: " + error.message());
  std::string actual = "absent";
  if (exists) {
    std::string digest;
    HM_ASSIGN_OR_RETURN(digest, file_sha256(directory / "left_calibration.json", 1024 * 1024, true));
    actual = "sha256:" + digest;
  }
  if (!source.akaze_calibration_fingerprint || *source.akaze_calibration_fingerprint != actual)
    return absl::FailedPreconditionError("Saved lens calibration changed since the original solve");
  return absl::OkStatus();
}

bool is_sha256(const std::string& value) {
  return value.size() == 64 &&
      std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

absl::StatusOr<StitchingReframeIntent> validate_intent(const fs::path& directory, const YAML::Node& effective) {
  const YAML::Node state = calibration(effective);
  const YAML::Node record = node_at(state, {"reframe"});
  if (!record || !record.IsMap() || canonical_text(record).size() > kMaximumIntentBytes || !record["version"] ||
      record["version"].as<int>() != 1)
    return absl::InvalidArgumentError("Invalid or unsupported stitching reframe intent");
  for (const char* key : {"source_generation", "source_owner", "desired_owner", "source_diagnostics"}) {
    if (!record[key] || !record[key].IsScalar() ||
        (std::string(key) != "source_diagnostics" && record[key].as<std::string>().empty()))
      return absl::InvalidArgumentError(std::string("Reframe intent has no ") + key);
  }
  for (const char* key : {"source_solver", "source_view", "source_backend_generation", "desired_view"}) {
    if (!record[key] || !record[key].IsMap())
      return absl::InvalidArgumentError(std::string("Reframe intent has invalid ") + key);
  }
  const YAML::Node images = record["source_image_sha256"];
  if (!images || !images.IsSequence() || images.size() != 2)
    return absl::InvalidArgumentError("Reframe intent must identify both representative PNGs");
  StitchingReframeIntent result;
  result.desired_owner = record["desired_owner"].as<std::string>();
  result.source_generation = record["source_generation"].as<std::string>();
  HM_RETURN_IF_ERROR(validate_pending_stitching_invalidation(effective, result.desired_owner));
  const auto stale = text_or(node_at(state, {"stale_from"}));
  if (!stale.empty() && stale != "canvas")
    return absl::FailedPreconditionError("An input/features invalidation cannot reuse a saved alignment");
  Snapshot current;
  HM_ASSIGN_OR_RETURN(current, snapshot(effective));
  HM_RETURN_IF_ERROR(supported(current));
  if (canonical_text(current.solver) != canonical_text(record["source_solver"]) ||
      canonical_text(current.view) != canonical_text(record["desired_view"]))
    return absl::FailedPreconditionError(
        "Stitching reframe settings or selected inputs changed after the request was saved");
  YAML::Node original = YAML::Clone(effective);
  const YAML::Node source_view = record["source_view"];
  original["stitching"]["projection"] = source_view["projection"];
  const auto source_projection = source_view["projection"].as<std::string>();
  const YAML::Node source_parameters = source_view["projection_parameters"];
  if (source_parameters.IsSequence() && source_parameters.size() == 0) {
    // Nonparameterized projections forbid even an explicit empty per-view
    // entry. Their normalized intent vector is empty, but restoring that
    // vector into canonical YAML must preserve the omitted-key convention.
    YAML::Node parameters = node_at(original, {"stitching", "projection_parameters"});
    if (parameters && parameters.IsMap())
      parameters.remove(source_projection);
  } else {
    original["stitching"]["projection_parameters"][source_projection] = source_parameters;
  }
  original["stitching"]["projection_framing"] = source_view["projection_framing"];
  original["stitching"]["max_output_width"] = source_view["max_output_width"];
  original["stitching"]["max_output_dimension"] = source_view["max_output_dimension"];
  original["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  original["hstream_ui"]["stitching_calibration"]["invalidation_id"] = record["source_owner"];
  original["hstream_ui"]["stitching_calibration"]["backend_generation"] = record["source_backend_generation"];
  Snapshot source;
  HM_ASSIGN_OR_RETURN(source, snapshot(original));
  HM_RETURN_IF_ERROR(
      validate_stitching_backend_generation(original, record["source_owner"].as<std::string>(), source.choices));
  std::string generation;
  HM_ASSIGN_OR_RETURN(generation, stitch_artifact_generation_id_locked(directory));
  if (generation != result.source_generation)
    return absl::AbortedError("The saved alignment generation was replaced; reframe request was preserved");
  HM_ASSIGN_OR_RETURN(result.source_provenance, source_provenance(directory));
  HM_RETURN_IF_ERROR(validate_provenance(result.source_provenance, source));
  HM_RETURN_IF_ERROR(validate_akaze_profile(directory, result.source_provenance));
  if (record["source_diagnostics"].as<std::string>() != result.source_provenance.calibration_frame_diagnostics)
    return absl::FailedPreconditionError("Reframe source representative diagnostics changed");
  for (size_t camera = 0; camera < 2; ++camera) {
    result.source_image_sha256[camera] = images[camera].as<std::string>();
    if (!is_sha256(result.source_image_sha256[camera]))
      return absl::InvalidArgumentError("Invalid representative PNG hash in reframe intent");
    std::string actual;
    HM_ASSIGN_OR_RETURN(actual, file_sha256(directory / (camera ? "right.png" : "left.png"), 512ULL * 1024 * 1024));
    if (actual != result.source_image_sha256[camera])
      return absl::AbortedError("A saved representative PNG changed; reframe request was preserved");
  }
  HM_RETURN_IF_ERROR(
      validate_selected_inputs(directory, effective, result.source_provenance, result.source_image_sha256));
  result.desired_choices = current.choices;
  result.desired_max_output_width = current.max_output_width;
  result.desired_max_output_dimension = current.max_output_dimension;
  return result;
}

} // namespace

bool HasStitchingReframeIntent(const YAML::Node& config) {
  const YAML::Node value = node_at(config, {"hstream_ui", "stitching_calibration", "reframe"});
  return value && !value.IsNull();
}

void ClearStitchingReframeIntent(YAML::Node& config) {
  YAML::Node state = calibration(config);
  if (state && state.IsMap())
    state.remove("reframe");
}

absl::StatusOr<StitchingReframeIntent> ValidateStitchingReframeIntentLocked(
    const fs::path& directory,
    const YAML::Node& config) {
  try {
    YAML::Node effective;
    HM_ASSIGN_OR_RETURN(effective, effective_config(config));
    return validate_intent(directory, effective);
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid stitching reframe intent: " + std::string(error.what()));
  }
}

absl::StatusOr<bool> PrepareStitchingReframeIntentLocked(
    const fs::path& directory,
    const YAML::Node& before_config,
    const YAML::Node& desired_config,
    const std::string& desired_owner,
    YAML::Node& desired_private) {
  try {
    const bool existing = HasStitchingReframeIntent(before_config);
    const auto previous_state = calibration(before_config);
    if (!existing && text_or(node_at(previous_state, {"status"})) != "complete")
      return false;
    YAML::Node before;
    YAML::Node desired;
    HM_ASSIGN_OR_RETURN(before, effective_config(before_config));
    HM_ASSIGN_OR_RETURN(desired, effective_config(desired_config));
    Snapshot original;
    Snapshot requested;
    HM_ASSIGN_OR_RETURN(original, snapshot(before, existing));
    HM_ASSIGN_OR_RETURN(requested, snapshot(desired, existing));
    // Existing intent must be valid even when a caller is about to replace it;
    // explicit full-clean/count operations instead use Clear directly.
    if (existing) {
      const auto valid = validate_intent(directory, before);
      if (!valid.ok())
        return valid.status();
    }
    YAML::Node comparable_original = YAML::Clone(original.solver);
    YAML::Node comparable_requested = YAML::Clone(requested.solver);
    if (!existing) {
      // A legacy missing budget/count cannot authorize capture, but must not
      // block unrelated playback or an explicit matcher/lens/source change.
      for (const char* key : {"control_points", "frame_count"}) {
        if (comparable_original[key].as<size_t>() == 0) {
          comparable_original.remove(key);
          comparable_requested.remove(key);
        }
      }
    }
    if (canonical_text(comparable_original) != canonical_text(comparable_requested)) {
      ClearStitchingReframeIntent(desired_private);
      return false;
    }
    if (!existing && canonical_text(original.view) == canonical_text(requested.view)) {
      // Both snapshots see the current process environment. Compare against
      // the published limits as well, or an environment-only canvas change
      // would fall through to the generic regeneration/cleanup path.
      const auto published = HuginProject::ReadCanvasProvenanceLocked(directory);
      if (!published.ok())
        return published.status();
      // Unchanged legacy playback does not acquire a new alignment request.
      // An explicit geometry edit still requires complete provenance below.
      if (!published->has_value() ||
          ((**published).max_canvas_dimension == requested.max_output_dimension &&
           (**published).max_output_width == requested.max_output_width))
        return false;
    }
    HM_ASSIGN_OR_RETURN(original, snapshot(before));
    HM_ASSIGN_OR_RETURN(requested, snapshot(desired));
    if (desired_owner.empty() || desired_owner.size() > 256)
      return absl::InvalidArgumentError("Reframe requires a bounded nonempty desired invalidation owner");
    HM_RETURN_IF_ERROR(supported(requested));
    YAML::Node record;
    if (existing) {
      record = YAML::Clone(node_at(previous_state, {"reframe"}));
    } else {
      if (original.choices.mapping_backend != "nona" || !original.choices.run_autooptimizer)
        return absl::FailedPreconditionError("The completed calibration has no optimized NONA alignment to reframe");
      if (text_or(node_at(previous_state, {"invalidation_id"})).empty())
        return absl::FailedPreconditionError("The completed calibration has no current generation owner");
      const auto source_owner = text_or(node_at(previous_state, {"backend_generation", "invalidation_id"}));
      if (source_owner.empty())
        return absl::FailedPreconditionError("The completed calibration has no original backend generation owner");
      // Unchanged Play reserves a fresh runtime owner before starting its
      // runner. If launch fails, the completed backend claim still identifies
      // the original solve. Validate that claim against the complete pre-edit
      // tuple and published provenance, independently of the runtime token.
      // Pending inputs/features never reach this completed-source branch.
      YAML::Node completed_source = YAML::Clone(before);
      completed_source["hstream_ui"]["stitching_calibration"]["invalidation_id"] = source_owner;
      HM_RETURN_IF_ERROR(validate_stitching_backend_generation(completed_source, source_owner, original.choices));
      HuginProject::CanvasProvenance source;
      HM_ASSIGN_OR_RETURN(source, source_provenance(directory));
      HM_RETURN_IF_ERROR(validate_provenance(source, original));
      HM_RETURN_IF_ERROR(validate_akaze_profile(directory, source));
      record["version"] = 1;
      std::string generation;
      HM_ASSIGN_OR_RETURN(generation, stitch_artifact_generation_id_locked(directory));
      record["source_generation"] = generation;
      record["source_owner"] = source_owner;
      record["source_solver"] = original.solver;
      record["source_view"] = original.view;
      record["source_view"]["max_output_width"] = source.max_output_width;
      record["source_view"]["max_output_dimension"] = source.max_canvas_dimension;
      record["source_backend_generation"] = YAML::Clone(node_at(previous_state, {"backend_generation"}));
      record["source_diagnostics"] = source.calibration_frame_diagnostics;
      std::array<std::string, 2> hashes;
      for (size_t camera = 0; camera < 2; ++camera) {
        HM_ASSIGN_OR_RETURN(
            hashes[camera], file_sha256(directory / (camera ? "right.png" : "left.png"), 512ULL * 1024 * 1024));
        record["source_image_sha256"].push_back(hashes[camera]);
      }
      HM_RETURN_IF_ERROR(validate_selected_inputs(directory, before, source, hashes));
    }
    record["desired_owner"] = desired_owner;
    record["desired_view"] = requested.view;
    if (canonical_text(record).size() > kMaximumIntentBytes)
      return absl::InvalidArgumentError("Reframe intent exceeds its bounded size");
    desired_private["hstream_ui"]["stitching_calibration"]["reframe"] = record;
    return true;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Cannot prepare stitching reframe: " + std::string(error.what()));
  }
}

} // namespace hm::stitching
