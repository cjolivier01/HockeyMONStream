#pragma once

#include <QtCore/QString>
#include <functional>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace hm::ui {
struct DetectorModel {
  QString id;
  QString label;
  QString config_prefix;
};
struct DetectorSelection {
  QString model;
  QString precision;
  QString prepared_engine;
  QString prepared_manifest;
  QString prepared_model;
};
using DetectorConfigPath = std::function<QString(const QString&)>;

// Both interfaces use the effective native configuration to distinguish bundled
// selections from custom engines. Empty precision means preserve that config.
std::vector<DetectorModel> ReadDetectorModels(const YAML::Node& defaults);
DetectorSelection ReadDetectorSelection(
    const YAML::Node& effective,
    const std::vector<DetectorModel>& models,
    const DetectorConfigPath& config_path,
    bool hockeymon);
QString DetectorConfigName(const DetectorModel& model, const QString& precision);
QString DetectorEnginePath(
    const DetectorModel& model,
    const DetectorSelection& selection,
    const DetectorConfigPath& config_path);
} // namespace hm::ui
