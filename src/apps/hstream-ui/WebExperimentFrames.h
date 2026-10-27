#pragma once

#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <functional>

#include "src/apps/hstream-ui/StitchingExperimentBackend.h"
#include "src/apps/hstream-ui/WebJobSpec.h"

namespace hm::web {
// The adapter supplies its existing bounded/cancellable process owner. The
// baseline and candidate are independent copies confined to this request.
using ExperimentProcessRunner =
    std::function<void(const QString&, const QStringList&, const QString&, const QProcessEnvironment&)>;
StitchingExperimentPlayerSelection ScanExperimentFrames(
    const Request& request,
    const StitchingExperimentWorkspace& baseline,
    const StitchingExperimentWorkspace& candidate,
    const ExperimentProcessRunner& run);
} // namespace hm::web
