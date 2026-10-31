#pragma once

class QObject;
class QEvent;
class QWidget;

// Install once at application startup. This protects every window, including
// parentless dialogs and controls created after startup, regardless of focus.
void install_value_control_wheel_guard();

// Kept for independently hosted dialogs. A null root covers the application.
bool route_value_control_wheel(QWidget* root, QObject* watched, QEvent* event);
