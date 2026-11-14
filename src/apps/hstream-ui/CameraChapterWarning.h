#pragma once

#include <QtCore/QString>
#include <filesystem>

class QWidget;

// Advisory reads only; normal startup remains responsible for malformed configs.
QString CameraChapterWarning(const std::filesystem::path& game);
bool ConfirmCameraChapterWarning(QWidget* parent, const QString& warning);
