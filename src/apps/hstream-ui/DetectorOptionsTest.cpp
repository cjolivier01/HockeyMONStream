#include "src/apps/hstream-ui/DetectorOptions.h"

#include <QtCore/QTemporaryDir>
#include <fstream>
#include <iostream>

int main() {
  QTemporaryDir directory;
  if (!directory.isValid())
    return 1;
  const auto path = [&](const QString& name) { return directory.path() + "/" + name; };
  const auto defaults = YAML::Load(
      "hstream_ui: {detector_models: [{id: first, label: First, config_prefix: first}, "
      "{id: second, label: Second, config_prefix: second}]}\n"
      "pipeline: {primary-gie: {config-file: second_fp16.yaml}}\n");
  const auto models = hm::ui::ReadDetectorModels(defaults);
  for (const auto& model : models) {
    for (const QString& precision : {QString("fp32"), QString("fp16"), QString("bf16"), QString("int8")}) {
      const auto name = hm::ui::DetectorConfigName(model, precision);
      std::ofstream(path(name).toStdString())
          << "property: {model-engine-file: '" << name.toStdString() << ".engine'}\n";
      auto config = YAML::Load("pipeline: {primary-gie: {}}\n");
      config["pipeline"]["primary-gie"]["config-file"] = name.toStdString();
      const auto selected = hm::ui::ReadDetectorSelection(config, models, path, false);
      if (selected.model != model.id || selected.precision != precision ||
          hm::ui::DetectorEnginePath(model, selected, path) != name + ".engine")
        return 1;
    }
  }
  const auto inherited = hm::ui::ReadDetectorSelection(defaults, models, path, false);
  if (inherited.model != "second" || inherited.precision != "fp16")
    return 1;
  auto custom =
      YAML::Load("pipeline: {primary-gie: {config-file: second_fp16.yaml, model-engine-file: custom.engine}}\n");
  const auto untouched = hm::ui::ReadDetectorSelection(custom, models, path, false);
  if (untouched.model != "first" || !untouched.precision.isEmpty())
    return 1;
  auto prepared = YAML::Load(
      "pipeline: {primary-gie: {config-file: second_int8.yaml, model-engine-file: prepared.engine}}\n"
      "hstream_ui: {detector_int8: {model: second, engine: prepared.engine, manifest: prepared.json}}\n");
  const auto selected = hm::ui::ReadDetectorSelection(prepared, models, path, false);
  if (selected.model != "second" || selected.precision != "int8" ||
      hm::ui::DetectorEnginePath(models.back(), selected, path) != "prepared.engine")
    return 1;
  prepared["hstream_ui"]["detector_int8"]["model"] = "first";
  if (!hm::ui::ReadDetectorSelection(prepared, models, path, false).precision.isEmpty())
    return 1;
  const std::vector<hm::ui::DetectorModel> legacy_models{
      {"default", "Default", "first"}, {"distilled-s", "Distilled", "second"}};
  for (const auto& [alias, index] : std::vector<std::pair<QString, std::size_t>>{{"yolov8m", 0}, {"yolov8s", 1}}) {
    prepared["hstream_ui"]["detector_int8"]["model"] = alias.toStdString();
    prepared["pipeline"]["primary-gie"]["config-file"] =
        hm::ui::DetectorConfigName(legacy_models[index], "int8").toStdString();
    const auto legacy = hm::ui::ReadDetectorSelection(prepared, legacy_models, path, false);
    if (legacy.precision != "int8" || legacy.model != legacy_models[index].id ||
        legacy.prepared_model != legacy_models[index].id)
      return 1;
  }
  auto python = YAML::Load("hstream_ui: {hockeymon: {detector: {model: second, precision: bf16}}}\n");
  const auto python_selected = hm::ui::ReadDetectorSelection(python, models, path, true);
  if (python_selected.model != "second" || python_selected.precision != "bf16")
    return 1;
  try {
    hm::ui::ReadDetectorModels(YAML::Load("{}"));
    return 1;
  } catch (const std::exception&) {
  }
  try {
    hm::ui::ReadDetectorSelection(YAML::Load("{}"), models, path, false);
    return 1;
  } catch (const std::exception&) {
  }
  std::cout << "PASS: shared detector families, inherited config, custom engines and model-bound INT8\n";
  return 0;
}
