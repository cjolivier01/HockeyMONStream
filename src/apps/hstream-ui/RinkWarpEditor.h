#pragma once

#include <QtCore/QString>
#include "yaml-cpp/yaml.h"

// Creates a self-contained, user-private still editor snapshot. The browser
// needs no server and cannot write game state. Returns the local HTML path.
QString WriteRinkWarpEditor(
    const QString& image_path,
    const QString& mask_path,
    const YAML::Node& baseline,
    const QString& cache_directory);
