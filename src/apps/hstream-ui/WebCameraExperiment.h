#pragma once

#include "hstream/src/libs/playtracker_replay/ReplaySession.h"
#include "src/apps/hstream-ui/WebJobSpec.h"

#include <functional>

namespace hm::web {
// Browser legacy bundles contain only regular files in one private directory.
void ValidateLegacyCameraFiles(const std::filesystem::path& manifest);

// Publication must accept the complete batch before any new trial is selectable.
// A size or storage failure retains the preceding native/rink comparison views.
void CommitCameraExperimentTrials(
    std::vector<hm::playtracker_replay::TrialResult>& trials,
    std::vector<hm::playtracker_replay::TrialResult> additions,
    const std::function<void()>& publish);

// One supervised camera dialog. Inputs are immutable worker-staged references;
// runtime commands carry the run/session/request identity and bounded values.
QJsonObject RunCameraExperiment(
    const Request& request,
    QJsonArray& artifacts,
    unsigned preview_port,
    const std::function<bool()>& interrupted);
} // namespace hm::web
