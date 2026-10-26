#include "src/apps/hstream-ui/WebCalibrationControls.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUuid>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/AutoInputLens.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"
#include "hstream/src/libs/stitching/StitchingCalibrationControls.h"
#include "hstream/src/libs/stitching/StitchingReframe.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace hm::web {
namespace {
namespace fs = std::filesystem;
namespace st = hm::stitching;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void check(const absl::Status& status) {
  require(status.ok(), status.ToString());
}
template <class T>
T take(absl::StatusOr<T> result) {
  check(result.status());
  return std::move(*result);
}
QString text(const std::string& value) {
  return QString::fromStdString(value);
}
QString hash(const std::string& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex());
}
YAML::Node at(const YAML::Node& root, std::initializer_list<const char*> path) {
  YAML::Node value = root;
  for (const auto* key : path) {
    if (!value.IsDefined() || !value.IsMap())
      return YAML::Node(YAML::NodeType::Undefined);
    const auto child = static_cast<const YAML::Node&>(value)[key];
    if (!child.IsDefined())
      return YAML::Node(YAML::NodeType::Undefined);
    value.reset(child);
  }
  return value;
}
void merge(YAML::Node target, const YAML::Node& source) {
  for (const auto& entry : source) {
    const auto key = entry.first.as<std::string>();
    if (entry.second.IsMap() && target[key].IsMap())
      merge(target[key], entry.second);
    else
      target[key] = YAML::Clone(entry.second);
  }
}

const std::set<QString>& control_roots() {
  static const std::set<QString> roots{
      "stitching.control_point_limit",
      "stitching.calibration_frame_count",
      "stitching.stitch_frame_time",
      "stitching.sync_method",
      "stitching.control_point_matcher",
      "stitching.control_point_resolution",
      "stitching.control_point_filtering",
      "stitching.control_point_execution_provider",
      "stitching.mapping_backend",
      "stitching.camera_config",
      "stitching.camera_fov",
      "stitching.camera_automation",
      "stitching.input_lens",
      "stitching.auto_input_lens_frozen",
      "stitching.run_autooptimizer",
      "stitching.projection",
      "stitching.projection_parameters",
      "stitching.projection_framing",
      "stitching.max_output_width",
      "stitching.post_stitch_rotate_degrees",
      "stitching.rink_config",
      "stitching.rink_mask_frame_time",
      "stitching.rink_mask_frame_time_default",
      "stitching.blend_mode",
      "stitching.blend_feather_fraction",
      "stitching.dtype",
      "rink.camera.fixed_edge_rotation_angle",
      "hstream_ui.stitching_calibration.control_points",
      "hstream_ui.stitching_calibration.frame_count",
      "hstream_ui.show_crop_dialog",
      "hstream_ui.show_leveling_dialog"};
  return roots;
}
void validate_controls(const QJsonObject& object, const QString& prefix = {}, size_t depth = 0) {
  static const std::map<QString, std::set<QString>> map_fields{
      {"stitching.control_point_filtering", {"epipolar_ransac", "homography_consistency"}},
      {"stitching.camera_fov", {"horizontal_fov", "vertical_fov"}},
      {"stitching.camera_automation", {"fov", "lens"}},
      {"stitching.input_lens", {"projection", "optimization", "distortion", "center_offset"}},
      {"stitching.projection_framing",
       {"auto_fov", "horizontal_fov", "auto_canvas", "auto_crop", "rotation_degrees", "crop"}}};
  require(depth <= 24, "Calibration controls are too deeply nested");
  for (auto item = object.begin(); item != object.end(); ++item) {
    require(!item.key().isEmpty() && !item.key().contains('.'), "Calibration control keys must be canonical");
    const QString path = prefix.isEmpty() ? item.key() : prefix + "." + item.key();
    bool selected = false, parent = false;
    for (const auto& root : control_roots()) {
      selected = selected || path == root || path.startsWith(root + ".");
      parent = parent || root.startsWith(path + ".");
    }
    require(selected || parent, "Unsupported calibration control: " + path.toStdString());
    if (path == "rink.camera.fixed_edge_rotation_angle") {
      require(
          item->isNull() || item->isDouble() ||
              (item->isArray() && item->toArray().size() == 2 && item->toArray()[0].isDouble() &&
               item->toArray()[1].isDouble()),
          "Fixed-edge angles must be null, numeric or [left, right]");
    }
    const auto fields = map_fields.find(prefix);
    if (fields != map_fields.end())
      require(
          fields->second.count(item.key()) && !item->isObject(),
          "Unsupported calibration control field: " + path.toStdString());
    if (prefix == "stitching.projection_parameters") {
      take(st::ParseStitchProjection(item.key().toStdString()));
      require(item->isArray(), "Projection parameters must be numeric arrays");
      for (const auto& number : item->toArray())
        require(number.isDouble(), "Projection parameters must be numeric arrays");
    }
    if (item->isObject()) {
      require(
          !selected || map_fields.count(path) || path == "stitching.projection_parameters",
          "Scalar calibration control cannot contain nested fields: " + path.toStdString());
      validate_controls(item->toObject(), path, depth + 1);
    } else
      require(selected, "Calibration control group must be an object");
  }
}

// Form snapshots describe changed leaves, not the entire saved view. In
// particular an unchanged form default must not overwrite editor-only framing.
void apply_changes(YAML::Node target, const QJsonObject& before, const QJsonObject& desired) {
  std::set<QString> names;
  for (auto item = before.begin(); item != before.end(); ++item)
    names.insert(item.key());
  for (auto item = desired.begin(); item != desired.end(); ++item)
    names.insert(item.key());
  for (const auto& name : names) {
    const auto previous = before.value(name), next = desired.value(name);
    if (previous == next)
      continue;
    const auto key = name.toStdString();
    if (next.isObject() || (next.isUndefined() && previous.isObject())) {
      if (!target[key].IsMap())
        target[key] = YAML::Node(YAML::NodeType::Map);
      apply_changes(target[key], previous.toObject(), next.toObject());
      if (next.isUndefined() && !target[key].size())
        target.remove(key);
    } else if (next.isUndefined()) {
      target.remove(key);
    } else {
      const auto wrapped = QJsonDocument(QJsonObject{{"value", next}}).toJson(QJsonDocument::Compact);
      target[key] = YAML::Load(wrapped.toStdString())["value"];
    }
  }
}

std::set<fs::path> authorized_media(const Request& request) {
  require(request.authorized_media.size() <= 10000, "Too many authorized calibration inputs");
  std::set<fs::path> allowed;
  for (const auto& value : request.authorized_media) {
    require(value.isString(), "Invalid calibration media authorization");
    const fs::path path(value.toString().toStdString());
    require(path.is_absolute() && fs::is_regular_file(path), "Authorized calibration media must be a regular file");
    allowed.insert(fs::canonical(path));
  }
  size_t entries = 0;
  for (fs::recursive_directory_iterator entry(request.game), end; entry != end; ++entry) {
    require(++entries <= 100000, "Calibration workspace exceeds its entry limit");
    if (entry->is_symlink())
      require(
          fs::is_regular_file(entry->path()) && allowed.count(fs::canonical(entry->path())),
          "Calibration alias is not an authorized media input");
  }
  return allowed;
}

fs::path media_alias(const Request& request, const QString& name, const std::set<fs::path>& allowed) {
  const fs::path relative(name.toStdString());
  require(
      !relative.empty() && !relative.is_absolute() && relative.lexically_normal() == relative &&
          *relative.begin() != "..",
      "Calibration media alias must stay inside its private game");
  const auto path = request.game / relative;
  require(fs::canonical(path.parent_path()) == path.parent_path(), "Calibration media alias has an indirect parent");
  require(fs::is_regular_file(path) && allowed.count(fs::canonical(path)), "Calibration media alias is not authorized");
  return fs::canonical(path);
}

bool apply_sources(const Request& request, YAML::Node config, const std::set<fs::path>& allowed) {
  const auto rows = request.payload.value("sources").toArray();
  if (rows.isEmpty())
    return false;
  require(rows.size() <= 10000, "Too many calibration source bindings");
  std::set<QString> ids, old_aliases, new_aliases;
  std::map<std::string, std::vector<std::string>> before, after;
  bool changed = false;
  for (const auto& row : rows) {
    require(row.isObject(), "Invalid calibration source binding");
    const auto item = row.toObject();
    require(item.size() == 5, "Unexpected calibration source binding fields");
    const auto id = RequiredString(item, "id", 128);
    const auto previous_role = RequiredString(item, "before_role", 16),
               next_role = RequiredString(item, "after_role", 16);
    require(
        (previous_role == "left" || previous_role == "right") && (next_role == "left" || next_role == "right"),
        "Calibration source roles must be left or right");
    const auto previous_alias = RequiredString(item, "before_alias"), next_alias = RequiredString(item, "after_alias");
    require(
        previous_role != next_role || previous_alias == next_alias,
        "Same-role calibration sources must retain their admitted private alias");
    require(
        ids.insert(id).second && old_aliases.insert(previous_alias).second && new_aliases.insert(next_alias).second,
        "Calibration source identities must be unique");
    require(
        media_alias(request, previous_alias, allowed) == media_alias(request, next_alias, allowed),
        "Calibration source staging cannot replace recording bytes");
    before[previous_role.toStdString()].push_back(previous_alias.toStdString());
    after[next_role.toStdString()].push_back(next_alias.toStdString());
    changed = changed || previous_role != next_role;
  }
  auto roles = at(config, {"hstream_ui", "video_roles"});
  if (!at(roles, {"left"}).IsSequence() && !at(roles, {"right"}).IsSequence())
    roles.reset(at(config, {"game", "videos"}));
  for (const char* role : {"left", "right"}) {
    require(!before[role].empty() && !after[role].empty(), "Both source roles require complete ordered chapters");
    const auto saved = at(roles, {role});
    require(
        saved.IsSequence() && saved.as<std::vector<std::string>>() == before[role],
        "Calibration source snapshot or chapter order changed");
    config["hstream_ui"]["video_roles"][role] = after[role];
    config["game"]["videos"][role] = after[role];
  }
  return changed;
}

void remove_native_aliases(YAML::Node config, const QJsonObject& before, const QJsonObject& desired) {
  const auto changed = [&](const char* group, const char* key) {
    return before.value(group).toObject().value(key) != desired.value(group).toObject().value(key);
  };
  const bool width = changed("stitching", "max_output_width");
  const bool rotation = changed("stitching", "post_stitch_rotate_degrees");
  const bool blend_mode = changed("stitching", "blend_mode");
  const bool blend_feather = changed("stitching", "blend_feather_fraction");
  const auto old_camera = before.value("rink").toObject().value("camera").toObject();
  const auto new_camera = desired.value("rink").toObject().value("camera").toObject();
  const bool angles = old_camera.value("fixed_edge_rotation_angle") != new_camera.value("fixed_edge_rotation_angle");
  for (const char* stage : {"hmstitcher", "hmplaycropper", "ds-playtracker"}) {
    auto node = at(config, {"pipeline", stage});
    if (!node.IsMap())
      continue;
    for (const char* section : {"", "properties", "private-properties"}) {
      auto properties = *section ? at(node, {section}) : node;
      if (!properties.IsMap())
        continue;
      for (const char* key :
           {"max-output-width",
            "max_output_width",
            "stitch-max-output-width",
            "stitch_max_output_width",
            "post-stitch-rotate-degrees",
            "post_stitch_rotate_degrees",
            "fixed-edge-rotation-angle",
            "fixed_edge_rotation_angle",
            "fixed-edge-rotation-angle-left",
            "fixed_edge_rotation_angle_left",
            "fixed-edge-rotation-angle-right",
            "fixed_edge_rotation_angle_right",
            "blend-mode",
            "blend_mode",
            "blend-feather-fraction",
            "blend_feather_fraction"})
        if ((width && std::string(key).find("width") != std::string::npos) ||
            (rotation && std::string(key).rfind("post", 0) == 0) ||
            (angles && std::string(key).rfind("fixed", 0) == 0) ||
            (blend_mode && (std::string(key) == "blend-mode" || std::string(key) == "blend_mode")) ||
            (blend_feather &&
             (std::string(key) == "blend-feather-fraction" || std::string(key) == "blend_feather_fraction")))
          properties.remove(key);
    }
  }
}
} // namespace

void ApplySavedCalibrationControlEdits(YAML::Node& config, const QJsonObject& before, const QJsonObject& desired) {
  validate_controls(before);
  validate_controls(desired);
  const auto baseline = take(hm::baseline_config::load()).values;
  auto previous_defaults = YAML::Clone(baseline);
  merge(previous_defaults, YAML::Load(QJsonDocument(before).toJson().toStdString()));
  auto previous_effective = YAML::Clone(previous_defaults);
  merge(previous_effective, config);
  previous_effective["stitching"]["max_output_width"] =
      take(st::ResolveStitchingCalibrationMaxWidth(config, previous_defaults));
  st::PreserveLegacyInputLensSelection(previous_effective, config);
  const auto saved = take(st::ReadStitchingCalibrationState(config, previous_effective));

  auto prepared = YAML::Clone(config);
  apply_changes(prepared, before, desired);
  remove_native_aliases(prepared, before, desired);
  const auto previous_stitching = before.value("stitching").toObject();
  const auto desired_stitching = desired.value("stitching").toObject();
  const auto previous_state = before.value("hstream_ui").toObject().value("stitching_calibration").toObject();
  const auto desired_state = desired.value("hstream_ui").toObject().value("stitching_calibration").toObject();
  if (previous_stitching.value("calibration_frame_count") == desired_stitching.value("calibration_frame_count"))
    prepared["stitching"]["calibration_frame_count"] =
        previous_state.value("frame_count") != desired_state.value("frame_count") &&
            !desired_state.value("frame_count").isUndefined()
        ? at(prepared, {"hstream_ui", "stitching_calibration", "frame_count"}).as<int>()
        : saved.controls.frame_count;
  auto defaults = YAML::Clone(baseline);
  merge(defaults, YAML::Load(QJsonDocument(desired).toJson().toStdString()));
  auto effective = YAML::Clone(defaults);
  merge(effective, prepared);
  effective["stitching"]["max_output_width"] = take(st::ResolveStitchingCalibrationMaxWidth(prepared, defaults));
  st::PreserveLegacyInputLensSelection(effective, prepared);
  if (previous_stitching.value("control_point_limit") != desired_stitching.value("control_point_limit"))
    effective["hstream_ui"]["stitching_calibration"]["control_points"] =
        at(effective, {"stitching", "control_point_limit"}).as<int>();
  take(
      st::ApplyStitchingCalibrationInputEdits(
          saved, take(st::ReadStitchingCalibrationControls(effective)), false, false, prepared));
  config = std::move(prepared);
  // A source-bound explicit release arrives as auto_input_lens_frozen=false.
  // Manual FOV edits alone must not release a frozen automatic lens component.
}

QJsonObject RunCalibrationControlsStage(const Request& request, const std::function<bool()>& cancelled) {
  const auto& payload = request.payload;
  require(
      payload.size() == 6 && payload.value("before_controls").isObject() &&
          payload.value("desired_controls").isObject() && payload.value("defaults").isObject() &&
          payload.value("sources").isArray(),
      "Calibration staging requires exact source identities, frozen controls/defaults and source bindings");
  // source identity is one object so its generation and config bytes cannot be
  // confused with the new run's owner or its post-cleanup artifact set.
  const auto source = payload.value("source").toObject();
  const bool cleaned_source = source.value("cleaned").isBool() && source.value("cleaned").toBool();
  require(
      source.size() == (cleaned_source ? 3 : 2),
      "Calibration source requires generation, config_sha256 and optional verified cleanup");
  std::optional<std::string> source_generation;
  if (!source.value("generation").isNull())
    source_generation = RequiredString(source, "generation", 1024 * 1024).toStdString();
  const auto source_hash = RequiredString(source, "config_sha256", 64);
  require(QRegularExpression("^[0-9a-f]{64}$").match(source_hash).hasMatch(), "Invalid calibration source config hash");
  const auto previous_controls = payload.value("before_controls").toObject(),
             desired_controls = payload.value("desired_controls").toObject();
  validate_controls(previous_controls);
  validate_controls(desired_controls);
  validate_controls(payload.value("defaults").toObject());
  require(payload.value("version").toInt() == 1, "Unsupported calibration staging version");
  const auto stop = [&] { require(!cancelled || !cancelled(), "Calibration staging cancelled"); };
  stop();
  const auto allowed = authorized_media(request);
  const auto baseline = take(hm::baseline_config::load()).values;
  auto before_defaults = YAML::Clone(baseline);
  const auto source_controls = YAML::Load(QJsonDocument(previous_controls).toJson().toStdString());
  merge(before_defaults, source_controls);
  st::PreserveLegacyInputLensSelection(before_defaults, source_controls);
  auto defaults = YAML::Clone(baseline);
  merge(defaults, YAML::Load(QJsonDocument(payload.value("defaults").toObject()).toJson().toStdString()));
  const auto owner = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  st::StitchingCalibrationPlan plan;
  YAML::Node prepared;
  bool output_changed = false;
  {
    auto artifacts = take(st::lock_canvas_constraint_artifacts(request.game));
    auto transaction = take(st::GameConfigTransactionLock::Acquire(request.game));
    stop();
    const auto bytes = take(
        st::read_bounded_regular_file_no_follow(request.game / "config.yaml", 4 * 1024 * 1024, "calibration config"));
    require(hash(bytes) == source_hash, "Calibration source config changed before staging");
    auto original = YAML::Load(bytes);
    const auto output_generation = at(original, {"rink", "stitched_output_generation"});
    require(
        source_generation ? output_generation.IsScalar() && output_generation.as<std::string>() == *source_generation
                          : !output_generation.IsDefined() || output_generation.IsNull(),
        "Calibration staging requires the exact saved output generation or its absence");
    const auto status = at(original, {"hstream_ui", "stitching_calibration", "status"});
    require(
        (cleaned_source ? !status.IsDefined() && !source_generation
                        : status.IsScalar() && status.as<std::string>() == "complete") &&
            !st::HasStitchingReframeIntent(original),
        "Calibration staging requires completed calibration or an explicitly cleaned source without a pending reframe");
    const auto rink = at(original, {"rink"});
    for (const auto& item : rink)
      require(
          item.first.as<std::string>().rfind("stitched_output_pending_", 0) != 0,
          "Calibration staging cannot adopt a pending live authorization");
    if (cleaned_source) {
      for (const char* name :
           {"stitching_generation_id",
            "stitching_canvas_provenance",
            "autooptimiser_out.pto",
            "hm_project.pto",
            "panorama.tif",
            "seam_file.png"})
        require(!fs::exists(request.game / name), "Cleaned calibration still contains retired solve artifacts");
      auto pruned = YAML::Clone(original);
      st::RemoveManualStitchingCleanConfigKeys(pruned);
      require(YAML::Dump(pruned) == YAML::Dump(original), "Cleaned calibration still contains retired solve state");
    } else {
      const auto generation = take(st::stitch_artifact_preflight_generation_id_locked(request.game));
      if (source_generation)
        check(st::validate_stitched_output_generation_hugin(*source_generation, generation));
    }
    // Authenticate the saved tuple against its own frozen form, never the
    // account defaults admitted for the next run. Private saved geometry wins
    // over that lower layer, including hidden crop and leveling controls.
    auto before_effective = YAML::Clone(before_defaults);
    merge(before_effective, original);
    before_effective["stitching"]["max_output_width"] =
        take(st::ResolveStitchingCalibrationMaxWidth(original, before_defaults));
    st::PreserveLegacyInputLensSelection(before_effective, original);
    auto before = take(st::ReadStitchingCalibrationState(original, before_effective));
    // Clean retires the saved alignment. A newly resolved automatic lens or
    // another geometry edit must not narrow that fresh solve to the canvas.
    // Seed its earliest stage without forcing release of retained input evidence.
    if (cleaned_source)
      before.stale_from = "input";
    const auto stale = at(original, {"hstream_ui", "stitching_calibration", "stale_from"});
    require(
        !stale.IsDefined() || stale.IsNull() || (stale.IsScalar() && stale.as<std::string>().empty()),
        "Calibration staging cannot adopt stale calibration artifacts");
    const auto claim = at(original, {"hstream_ui", "stitching_calibration", "backend_generation"});
    if (claim.IsDefined() && !claim.IsNull()) {
      const auto saved_owner = at(original, {"hstream_ui", "stitching_calibration", "invalidation_id"});
      require(
          saved_owner.IsScalar() && !saved_owner.as<std::string>().empty(),
          "Calibration staging requires the existing backend claim's owner");
      // Saved-view reframes may intentionally retire the rendered output
      // identity. Their original algorithm claim still binds the resolved
      // controls; never replace a conflicting claim with the new launch owner.
      const auto& controls = before.controls;
      st::StitchingBackendChoices choices;
      choices.control_point_matcher = controls.matcher;
      choices.mapping_backend = controls.backend;
      choices.projection = controls.projection;
      choices.run_autooptimizer = controls.autooptimizer;
      choices.projection_parameters = controls.projection_parameters;
      choices.projection_framing = controls.framing;
      choices.camera = controls.camera;
      choices.control_point_resolution = take(st::read_control_point_resolution(before_effective));
      choices.control_point_execution_provider = take(st::read_control_point_execution_provider(before_effective));
      choices.control_point_filtering = controls.filtering;
      choices.calibration_frame_selection_fingerprint = take(st::player_frame_selection_fingerprint(before_effective));
      choices.manual_control_point_fingerprint = take(st::manual_control_point_fingerprint(before_effective));
      check(st::validate_stitching_backend_generation(before_effective, saved_owner.as<std::string>(), choices));
    }
    before_effective["stitching"]["calibration_frame_count"] = before.controls.frame_count;
    prepared = YAML::Clone(original);
    const auto previous_stitching = previous_controls.value("stitching").toObject();
    const auto desired_stitching = desired_controls.value("stitching").toObject();
    // Restore the user's inherited/explicit choice before changing its rink or
    // time; a generated prior-rink value is not an independent user override.
    for (const char* key : {"rink_config", "rink_mask_frame_time", "rink_mask_frame_time_default"}) {
      if (previous_stitching.value(key) != desired_stitching.value(key)) {
        st::restore_generated_rink_mask_frame_time(prepared);
        break;
      }
    }
    apply_changes(prepared, previous_controls, desired_controls);
    const auto previous_state =
        previous_controls.value("hstream_ui").toObject().value("stitching_calibration").toObject();
    const auto desired_state =
        desired_controls.value("hstream_ui").toObject().value("stitching_calibration").toObject();
    if (previous_stitching.value("calibration_frame_count") == desired_stitching.value("calibration_frame_count")) {
      // Legacy completed runs may only record the count in calibration state.
      // An unchanged form cannot replace their two inputs with baseline four.
      prepared["stitching"]["calibration_frame_count"] =
          previous_state.value("frame_count") != desired_state.value("frame_count") &&
              !desired_state.value("frame_count").isUndefined()
          ? at(prepared, {"hstream_ui", "stitching_calibration", "frame_count"}).as<int>()
          : before.controls.frame_count;
    }
    const bool sources_changed = apply_sources(request, prepared, allowed);
    remove_native_aliases(prepared, previous_controls, desired_controls);
    for (const char* key :
         {"generated_stitching_backend_choices",
          "generated_control_point_filtering",
          "generated_control_point_resolution",
          "generated_control_point_execution_provider",
          "generated_stitching_rink_context"})
      prepared["hstream_ui"].remove(key);
    auto effective = YAML::Clone(defaults);
    merge(effective, prepared);
    st::PreserveLegacyInputLensSelection(effective, prepared);
    effective["stitching"]["max_output_width"] = take(st::ResolveStitchingCalibrationMaxWidth(prepared, defaults));
    if (previous_stitching.value("control_point_limit") != desired_stitching.value("control_point_limit")) {
      const auto points = at(effective, {"stitching", "control_point_limit"}).as<int>();
      prepared["hstream_ui"]["stitching_calibration"]["control_points"] = points;
      effective["hstream_ui"]["stitching_calibration"]["control_points"] = points;
    }
    bool refresh_optics = sources_changed;
    const bool explicitly_released = desired_stitching.contains("auto_input_lens_frozen") &&
        desired_stitching.value("auto_input_lens_frozen").isBool() &&
        !desired_stitching.value("auto_input_lens_frozen").toBool();
    refresh_optics = refresh_optics || explicitly_released;
    for (const char* key : {"camera_config", "camera_fov", "camera_automation", "input_lens"})
      refresh_optics = refresh_optics || previous_stitching.value(key) != desired_stitching.value(key);
    if (refresh_optics) {
      auto camera = take(st::read_stitch_camera_selection(effective));
      // Qt retains a saved automatic component while editing the other manual
      // component. Profile/Auto edits release the snapshot even after an edit
      // is reverted; the supervisor binds that explicit intent to its source.
      if (sources_changed || explicitly_released ||
          previous_stitching.value("camera_config") != desired_stitching.value("camera_config") ||
          before.controls.camera.automatic_fov() != camera.automatic_fov() ||
          before.controls.camera.automatic_lens() != camera.automatic_lens())
        effective["stitching"]["auto_input_lens_frozen"] = false;
      else if (at(effective, {"stitching", "auto_input_lens_frozen"}).as<bool>(false)) {
        if (camera.automatic_fov()) {
          camera.horizontal_fov = before.controls.camera.horizontal_fov;
          camera.vertical_fov = before.controls.camera.vertical_fov;
        }
        if (camera.automatic_lens())
          camera.input_lens = before.controls.camera.input_lens;
        st::write_stitch_camera_selection(effective, camera);
      }
      auto optics_config = YAML::Clone(effective);
      if (camera.automatic_fov() || camera.automatic_lens()) {
        auto roles = at(effective, {"hstream_ui", "video_roles"});
        if (!at(roles, {"left"}).IsSequence() && !at(roles, {"right"}).IsSequence())
          roles.reset(at(effective, {"game", "videos"}));
        for (const char* role : {"left", "right"}) {
          const auto chapters = at(roles, {role});
          require(chapters.IsSequence() && chapters.size(), "Automatic optics require both admitted source playlists");
          for (const auto& chapter : chapters)
            media_alias(request, text(chapter.as<std::string>()), allowed);
          // Pin explicit validated playlists so metadata discovery cannot select
          // another directory or an absolute saved path during this preflight.
          optics_config["hstream_ui"]["video_roles"][role] = YAML::Clone(chapters);
        }
      }
      camera = st::ReadAutoInputLensForGame(request.game, optics_config, camera).camera;
      st::write_stitch_camera_selection(prepared, camera);
      st::write_stitch_camera_selection(effective, camera);
      prepared["stitching"]["auto_input_lens_frozen"] = true;
      effective["stitching"]["auto_input_lens_frozen"] = true;
    }
    const auto desired = take(st::ReadStitchingCalibrationControls(effective));
    bool canvas_required = false, canvas_cleanup = false;
    if (before.controls.max_output_width != desired.max_output_width) {
      const auto compatibility =
          take(st::check_canvas_constraint_metadata_locked(request.game, desired.max_output_width));
      canvas_required = !compatibility.artifacts_compatible;
      canvas_cleanup = canvas_required && compatibility.requires_regeneration;
    }
    plan = take(
        st::PrepareStitchingCalibrationControlsLocked(
            request.game,
            before,
            desired,
            before_effective,
            effective,
            owner,
            false,
            sources_changed,
            canvas_required,
            canvas_cleanup,
            prepared));
    const auto original_rotation = at(before_effective, {"stitching", "post_stitch_rotate_degrees"});
    const auto desired_rotation = at(effective, {"stitching", "post_stitch_rotate_degrees"});
    output_changed = previous_stitching.value("post_stitch_rotate_degrees") !=
            desired_stitching.value("post_stitch_rotate_degrees") ||
        YAML::Dump(original_rotation) != YAML::Dump(desired_rotation) ||
        take(st::read_rink_mask_frame_time(before_effective)) != take(st::read_rink_mask_frame_time(effective));
    if (plan.required || output_changed) {
      st::invalidate_rotation_dependent_artifacts(prepared);
      prepared["hstream_ui"]["stitching_calibration"]["rink_mask_status"] = "pending";
    }
    const auto serialized = YAML::Dump(prepared) + "\n";
    require(serialized.size() <= 4 * 1024 * 1024, "Staged calibration config exceeds its bound");
    stop();
    if (plan.required || output_changed)
      take(st::publish_game_config_without_rink_masks(request.game, serialized, true));
    else
      check(st::publish_game_config(request.game, serialized));
  }
  stop();
  if (plan.cleanup == st::StitchingCalibrationCleanup::kAll)
    check(st::clean_stitching_artifacts(request.game.string(), owner));
  else if (plan.cleanup == st::StitchingCalibrationCleanup::kControlPoints)
    check(st::clean_stitching_artifacts_from_control_points(request.game.string(), owner));
  stop();
  check(st::CompleteStitchingCalibrationCleanup(request.game, plan, prepared));
  stop();
  const auto bytes = take(
      st::read_bounded_regular_file_no_follow(
          request.game / "config.yaml", 4 * 1024 * 1024, "staged calibration config"));
  return {
      {"calibration_controls_stage_version", 1},
      {"staged", true},
      {"source", source},
      {"invalidation_id", text(owner)},
      {"pending", plan.required},
      {"start_stage", text(plan.start_stage)},
      {"cleanup", QString::fromLatin1(st::StitchingCalibrationCleanupName(plan.cleanup))},
      {"cleanup_completed", true},
      {"alignment_reused", plan.alignment_reused},
      {"retained_selected_frames", plan.retain_selected_frames},
      {"retained_manual_matches", plan.retain_manual_matches},
      {"output_changed", output_changed},
      {"config_sha256", hash(bytes)}};
}
} // namespace hm::web
