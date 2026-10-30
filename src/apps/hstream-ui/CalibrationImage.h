#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>

// Shared Qt/browser scoreboard proxy geometry; coordinates remain in the source canvas.
QSize scoreboardPreviewSize(const QSize& source_size);

// Bounded display proxies for saved calibration artifacts; no video readback.
QImage readCalibrationPng(const QString& path, const QSize& size);
QImage readCalibrationTiff(const QString& path, int maximum_dimension);
