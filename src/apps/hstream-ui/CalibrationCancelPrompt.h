#pragma once

#include <QtCore/QPointer>

#include <functional>

class QMessageBox;
class QString;
class QWidget;

// Shared confirmation for the calibration selectors whose cancel button stops
// the whole stitching calibration run rather than just that step. Keeping one
// copy keeps the two dialogs from describing the same consequence differently.
//
// `scope` names the step the operator might think is all that gets cancelled,
// and `alternative` names that dialog's way to continue the run instead. The
// prompt is window-modal and deletes itself when it closes. `owner` is set to
// the prompt, cleared before `confirmed` runs, and only has to stay valid as
// long as `parent` does, because `parent` is the connection's context.
void open_calibration_cancel_prompt(
    QWidget* parent,
    QPointer<QMessageBox>& owner,
    const QString& object_name,
    const QString& scope,
    const QString& alternative,
    std::function<void()> confirmed);
