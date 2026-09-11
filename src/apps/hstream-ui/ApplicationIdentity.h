#pragma once

#include <QtCore/QString>
#include <QtGui/QIcon>

namespace hm::ui_internal {

// Call before constructing QApplication so native windows and desktop entries
// share the same identity.
void configure_application_identity();
QIcon application_icon();

// Register an uninstalled command-line build with the Linux desktop before
// showing its first window. Package-owned and custom launchers take precedence.
// Returns an error message on failure; desktop integration is best effort.
QString ensure_desktop_integration(const QString& executable_path);

} // namespace hm::ui_internal
