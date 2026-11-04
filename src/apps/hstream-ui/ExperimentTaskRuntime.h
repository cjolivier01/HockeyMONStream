#pragma once

#include <yaml-cpp/yaml.h>
#include "src/apps/hstream-ui/WebExperimentFrames.h"

namespace hm::experiment {
using hm::web::ExperimentProcessRunner;
void merge_settings(YAML::Node destination, const YAML::Node& overlay);
QJsonObject camera_json(const hm::stitching::StitchCameraSelection& camera);
QJsonObject filtering_json(const hm::stitching::ControlPointFiltering& filtering);
StitchingExperimentSettings settings(const QJsonObject& object, const YAML::Node& source);
QJsonObject settings_json(const StitchingExperimentSettings& value);
StitchingExperimentWorkspace workspace(const hm::web::Request& request);
QJsonObject workspace_json(const hm::web::Request& request, const StitchingExperimentWorkspace& value);
bool materialized(const StitchingExperimentWorkspace& saved);
void run_experiment(
    const hm::web::Request& request,
    const StitchingExperimentWorkspace& saved,
    const ExperimentProcessRunner& run_process,
    bool complete = true,
    bool materialize = false);
} // namespace hm::experiment
