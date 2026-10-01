#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>

// Bounded display proxies for saved calibration artifacts; no video readback.
QImage readCalibrationPng(const QString& path, const QSize& size);
QImage readCalibrationTiff(const QString& path, int maximum_dimension);
