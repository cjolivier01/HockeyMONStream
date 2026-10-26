#include "src/apps/hstream-ui/DetectorOptions.h"

#include <QtCore/QFileInfo>
#include <algorithm>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::ui {
namespace {
const std::vector<QString> kPrecisions{"fp32", "fp16", "bf16", "int8"};
QString configured_string(const YAML::Node& config, const QString& key) {
  const auto value = hm::get_node(config, key.toStdString());
  return value ? QString::fromStdString(value->as<std::string>()) : QString();
}
} // namespace

std::vector<DetectorModel> ReadDetectorModels(const YAML::Node& defaults) {
  std::vector<DetectorModel> models;
  const auto configured = hm::get_node(defaults, "hstream_ui.detector_models");
  if (configured && configured->IsSequence()) {
    for (const YAML::Node& entry : *configured) {
      if (!entry.IsMap())
        continue;
      try {
        if (!entry["id"] || !entry["config_prefix"])
          continue;
        const QString id = QString::fromStdString(entry["id"].as<std::string>());
        const QString prefix = QString::fromStdString(entry["config_prefix"].as<std::string>());
        if (id.isEmpty() || prefix.isEmpty())
          continue;
        models.push_back({id, entry["label"] ? QString::fromStdString(entry["label"].as<std::string>()) : id, prefix});
      } catch (const YAML::Exception&) {
        // Preserve the other usable entries, as in the desktop catalog.
      }
    }
  }
  if (models.empty())
    throw std::runtime_error("The baseline must declare hstream_ui.detector_models");
  return models;
}

QString DetectorConfigName(const DetectorModel& model, const QString& precision) {
  if (std::find(kPrecisions.begin(), kPrecisions.end(), precision) == kPrecisions.end())
    throw std::runtime_error("Unsupported detector precision");
  return precision == "fp32" ? model.config_prefix + ".yaml"
                             : QString("%1_%2.yaml").arg(model.config_prefix, precision);
}

DetectorSelection ReadDetectorSelection(
    const YAML::Node& effective,
    const std::vector<DetectorModel>& models,
    const DetectorConfigPath& config_path,
    bool hockeymon) {
  if (models.empty())
    throw std::runtime_error("The detector catalog is empty");
  DetectorSelection result;
  result.model = models.front().id;
  const QString prepared = hockeymon ? "hstream_ui.hockeymon.detector" : "hstream_ui.detector_int8";
  result.prepared_engine = configured_string(effective, prepared + ".engine");
  result.prepared_manifest = configured_string(effective, prepared + ".manifest");
  result.prepared_model = configured_string(effective, prepared + ".model");
  // Older web preparation receipts stored architecture names in this field.
  if (result.prepared_model == "yolov8m")
    result.prepared_model = "default";
  else if (result.prepared_model == "yolov8s")
    result.prepared_model = "distilled-s";
  // Before the catalog existed, preparations used its first detector.
  if (result.prepared_model.isEmpty() && !result.prepared_engine.isEmpty())
    result.prepared_model = models.front().id;
  if (hockeymon && hm::has_node(effective, prepared.toStdString() + ".precision", false)) {
    result.model = configured_string(effective, prepared + ".model");
    result.precision = configured_string(effective, prepared + ".precision");
    if (std::none_of(models.begin(), models.end(), [&](const auto& model) { return model.id == result.model; }) ||
        (!result.precision.isEmpty() &&
         std::find(kPrecisions.begin(), kPrecisions.end(), result.precision) == kPrecisions.end()))
      throw std::runtime_error("Invalid saved HockeyMON detector model or precision");
    return result;
  }
  const auto configured = hm::get_node(effective, "pipeline.primary-gie.config-file");
  if (!configured || !configured->IsScalar())
    throw std::runtime_error("The baseline must declare pipeline.primary-gie.config-file");
  const QString file = QString::fromStdString(configured->as<std::string>());
  for (const auto& model : models) {
    for (const auto& precision : kPrecisions) {
      const QString name = DetectorConfigName(model, precision);
      if (file != name && QFileInfo(file).absoluteFilePath() != QFileInfo(config_path(name)).absoluteFilePath())
        continue;
      const auto engine = hm::get_node(effective, "pipeline.primary-gie.model-engine-file");
      if (engine) {
        try {
          const auto inference = YAML::LoadFile(config_path(name).toStdString());
          const QString selected_engine = QString::fromStdString(engine->as<std::string>());
          const bool prepared_matches =
              precision == "int8" && result.prepared_model == model.id && selected_engine == result.prepared_engine;
          if (selected_engine != QString::fromStdString(inference["property"]["model-engine-file"].as<std::string>()) &&
              !prepared_matches)
            continue;
        } catch (const std::exception&) {
          continue;
        }
      }
      result.model = model.id;
      result.precision = precision;
      return result;
    }
  }
  return result;
}

QString DetectorEnginePath(
    const DetectorModel& model,
    const DetectorSelection& selection,
    const DetectorConfigPath& config_path) {
  if (selection.precision == "int8" && !selection.prepared_engine.isEmpty() && selection.prepared_model == model.id)
    return selection.prepared_engine;
  const auto inference = YAML::LoadFile(config_path(DetectorConfigName(model, selection.precision)).toStdString());
  return QString::fromStdString(inference["property"]["model-engine-file"].as<std::string>());
}
} // namespace hm::ui
