#include "src/apps/hstream-ui/MatchEditorDialog.h"

#include "src/apps/hstream-ui/ActionIcons.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <typename Widget>
Widget* widget(QWidget& dialog, const char* name) {
  auto* result = dialog.findChild<Widget*>(name);
  require(result != nullptr, "Missing editor control");
  return result;
}
bool wait_until(const std::function<bool()>& condition) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 2000) {
    QApplication::processEvents();
    if (condition())
      return true;
    QThread::msleep(5);
  }
  return condition();
}
void click(QGraphicsView* view, QPointF scene) {
  const QPoint pos = view->mapFromScene(scene);
  QMouseEvent press(
      QEvent::MouseButtonPress,
      pos,
      view->viewport()->mapToGlobal(pos),
      Qt::LeftButton,
      Qt::LeftButton,
      Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &press);
  QMouseEvent release(
      QEvent::MouseButtonRelease,
      pos,
      view->viewport()->mapToGlobal(pos),
      Qt::LeftButton,
      Qt::NoButton,
      Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &release);
}
} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  install_button_icon_style();
  try {
    QTemporaryDir root;
    require(root.isValid(), "Cannot create fixture directory");
    // Exercise native 16-bit input: editing must retain camera coordinates and
    // only convert the display image, never rewrite the immutable input PNG.
    QImage image(512, 256, QImage::Format_RGBA64);
    image.fill(QColor("#607080"));
    const QString path = root.path() + "/camera.png";
    require(image.save(path), "Cannot save 16-bit input");
    hm::stitching::CalibrationMatchSet automatic;
    automatic.fingerprint = "automatic";
    automatic.input_fingerprint = "shared-inputs";
    hm::stitching::CalibrationMatchFrame frame;
    frame.images = {path.toStdString(), path.toStdString()};
    frame.sizes = {cv::Size(512, 256), cv::Size(512, 256)};
    for (int i = 0; i < 10; ++i) {
      hm::stitching::FeatureMatch match;
      match.left = {34.25f + 38 * i, 40.125f + 12 * i};
      match.right = {38.75f + 38 * i, 44.5f + 12 * i};
      match.score = 0.75f;
      match.left_index = i;
      match.right_index = i + 10;
      frame.matches.push_back(match);
    }
    automatic.frames = {frame, frame};
    QWidget owner;
    owner.show();
    MatchEditorDialog dialog(automatic, automatic, &owner);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.resize(1280, 720);
    dialog.show();
    QApplication::processEvents();
    require(
        dialog.windowType() == Qt::Window && dialog.parentWidget() == &owner,
        "Editor must retain an owned, maximizable window");
    require(dialog.size() == QSize(1280, 720), "Editor controls must fit a 1280x720 window");
    for (auto* button : dialog.findChildren<QPushButton*>()) {
      const QRect rect(button->mapTo(&dialog, QPoint()), button->size());
      require(dialog.rect().contains(rect), "Editor must not clip toolbar controls at 1280x720");
      require(!button->icon().isNull(), "Every editor action must have a colored icon");
    }
    if (QGuiApplication::platformName() == "xcb") {
      require(
          dialog.windowHandle()->transientParent() == owner.windowHandle() &&
              dialog.windowModality() == Qt::WindowModal && QApplication::activeModalWidget() == &dialog,
          "Native editor window must retain its parent and modal relationship");
      // Mutter decorates and places newly mapped Xwayland windows
      // asynchronously. A single processEvents() can still expose Qt's initial
      // undecorated position, which is not the normal geometry users see. Wait
      // for exposed, decorated client/frame geometry to settle before recording
      // the exact rectangle that maximize/restore must preserve.
      QRect last_client;
      QRect last_frame;
      QElapsedTimer stable;
      stable.start();
      require(
          wait_until([&] {
            const QRect client = dialog.geometry();
            const QRect frame = dialog.frameGeometry();
            if (client != last_client || frame != last_frame) {
              last_client = client;
              last_frame = frame;
              stable.restart();
            }
            return dialog.windowHandle()->isExposed() && frame.height() > client.height() && stable.elapsed() >= 200;
          }),
          "Window manager must finish placing the editor before maximize testing");
      const QRect normal = dialog.geometry();
      qInfo() << "Editor normal geometry:" << normal << "frame:" << dialog.frameGeometry()
              << "normalGeometry:" << dialog.normalGeometry() << "DPR:" << dialog.devicePixelRatioF();
      auto* maximize = widget<QPushButton>(dialog, "matchEditorMaximize");
      maximize->click();
      require(
          wait_until([&] {
            const QSize available = dialog.screen()->availableGeometry().size();
            return dialog.isMaximized() && dialog.size() != normal.size() &&
                dialog.width() >= available.width() * 0.8 && dialog.height() >= available.height() * 0.8;
          }),
          "Window manager must physically maximize the match editor");
      maximize->click();
      const bool restored = wait_until([&] { return !dialog.isMaximized() && dialog.geometry() == normal; });
      qInfo() << "Editor restored geometry:" << dialog.geometry() << "frame:" << dialog.frameGeometry()
              << "normalGeometry:" << dialog.normalGeometry() << "maximized:" << dialog.isMaximized();
      require(restored, "Match editor restore must recover its original geometry");
    }
    const QString screenshot = qEnvironmentVariable("HSTREAM_TEST_MATCH_EDITOR_SCREENSHOT");
    if (!screenshot.isEmpty())
      require(dialog.grab().save(screenshot), "Cannot save editor screenshot");
    auto* save = widget<QPushButton>(dialog, "matchEditorSave");
    require(save->isEnabled(), "16-bit camera images should load");
    auto* left = widget<QGraphicsView>(dialog, "matchEditorLeft");
    auto* right = widget<QGraphicsView>(dialog, "matchEditorRight");
    const auto original = dialog.editedSet();
    click(left, {34.25, 40.125});
    require(
        dialog.editedSet().frames[0].matches[0].left == frame.matches[0].left,
        "Selecting an endpoint must not quantize it");
    auto* x = widget<QDoubleSpinBox>(dialog, "matchEditorCoordinate0");
    x->setValue(41.375);
    auto edited = dialog.editedSet();
    require(
        edited.frames[0].matches[0].left.x == 41.375f && edited.frames[0].matches[0].right == frame.matches[0].right,
        "Numeric edits must alter only the selected endpoint");
    require(
        edited.frames[0].matches[0].score == frame.matches[0].score && edited.frames[0].matches[0].left_index == 0,
        "Editing must retain match metadata");
    widget<QPushButton>(dialog, "matchEditorUndo")->click();
    require(
        dialog.editedSet().frames[0].matches[0].left == frame.matches[0].left,
        "Undo must restore exact original floats");
    widget<QPushButton>(dialog, "matchEditorRedo")->click();
    require(dialog.editedSet().frames[0].matches[0].left.x == 41.375f, "Redo must replay numeric edit");
    // Actual interaction moves one endpoint and commits one undoable operation.
    const QPoint start = left->mapFromScene({41.375, 40.125});
    const QPoint end = start + QPoint(24, 13);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        start,
        left->viewport()->mapToGlobal(start),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(left->viewport(), &press);
    QMouseEvent move(
        QEvent::MouseMove, end, left->viewport()->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(left->viewport(), &move);
    QMouseEvent release(
        QEvent::MouseButtonRelease,
        end,
        left->viewport()->mapToGlobal(end),
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(left->viewport(), &release);
    require(dialog.editedSet().frames[0].matches[0].left.x != 41.375f, "Dragging must move selected endpoint");
    widget<QPushButton>(dialog, "matchEditorUndo")->click();
    require(dialog.editedSet().frames[0].matches[0].left.x == 41.375f, "One undo must restore a complete drag");
    widget<QPushButton>(dialog, "matchEditorAdd")->click();
    require(!save->isEnabled(), "An incomplete correspondence cannot be saved");
    click(left, {200, 200});
    require(dialog.editedSet().frames[0].matches.size() == 10, "One endpoint must not create a partial match");
    click(right, {210, 210});
    require(
        dialog.editedSet().frames[0].matches.size() == 11 && save->isEnabled(),
        "Two endpoints must add a complete correspondence");
    widget<QPushButton>(dialog, "matchEditorDelete")->click();
    require(dialog.editedSet().frames[0].matches.size() == 10, "Delete must remove selected correspondence");
    widget<QPushButton>(dialog, "matchEditorUndo")->click();
    require(dialog.editedSet().frames[0].matches.size() == 11, "Undo must restore deleted correspondence");
    auto* pairs = widget<QComboBox>(dialog, "matchEditorPair");
    pairs->setCurrentIndex(1);
    x->setValue(63.5);
    require(
        dialog.editedSet().frames[1].matches[0].left.x == 63.5f && dialog.editedSet().frames[0].matches.size() == 11,
        "Pair editing must be independent");
    widget<QPushButton>(dialog, "matchEditorReset")->click();
    edited = dialog.editedSet();
    require(
        edited.frames[0].matches.size() == 10 && edited.frames[0].matches[0].left == frame.matches[0].left &&
            edited.frames[1].matches[0].left == frame.matches[0].left,
        "Reset must restore every original automatic pair");
    widget<QPushButton>(dialog, "matchEditorUndo")->click();
    edited = dialog.editedSet();
    require(
        edited.frames[0].matches.size() == 11 && edited.frames[1].matches[0].left.x == 63.5f,
        "Reset must be one undoable operation across pairs");
    require(
        edited.manual && edited.fingerprint.empty() && edited.input_fingerprint == original.input_fingerprint &&
            edited.automatic_fingerprint == original.automatic_fingerprint,
        "Edited set must retain input and automatic provenance");
    save->click();
    require(dialog.result() == QDialog::Accepted, "Save must accept the in-memory edit for caller publication");
    MatchEditorDialog cancellation(automatic, automatic, &owner);
    cancellation.show();
    QApplication::processEvents();
    require(
        widget<QLabel>(cancellation, "matchEditorStatus")->text().contains("20 total across 2 pairs"),
        "Editor must expose the complete correspondence count");
    widget<QDoubleSpinBox>(cancellation, "matchEditorCoordinate0")->setValue(92.125);
    bool kept = false;
    QTimer::singleShot(0, &cancellation, [&] {
      auto* prompt = cancellation.findChild<QMessageBox*>("matchEditorDiscardPrompt");
      if (!prompt)
        return;
      auto* keep = widget<QPushButton>(*prompt, "matchEditorKeepEditing");
      kept = prompt->defaultButton() == keep && !keep->icon().isNull();
      keep->click();
    });
    widget<QPushButton>(cancellation, "matchEditorCancel")->click();
    require(
        kept && cancellation.isVisible() && cancellation.editedSet().frames[0].matches[0].left.x == 92.125f,
        "Keep editing must preserve unsaved edits and leave the editor open");
    bool discarded = false;
    QTimer::singleShot(0, &cancellation, [&] {
      auto* prompt = cancellation.findChild<QMessageBox*>("matchEditorDiscardPrompt");
      if (!prompt)
        return;
      auto* discard = widget<QPushButton>(*prompt, "matchEditorDiscardChanges");
      discarded = !discard->icon().isNull();
      discard->click();
    });
    cancellation.close();
    require(
        discarded && !cancellation.isVisible() && cancellation.result() == QDialog::Rejected,
        "Title-bar close must confirm before discarding unsaved edits");
    // Undo can return the current set to the initial values even with a nonempty
    // history. Closing that state must not produce a false unsaved warning.
    MatchEditorDialog restored(automatic, automatic, &owner);
    restored.show();
    widget<QDoubleSpinBox>(restored, "matchEditorCoordinate0")->setValue(78.25);
    widget<QPushButton>(restored, "matchEditorUndo")->click();
    bool unexpected_prompt = false;
    QTimer::singleShot(0, &restored, [&] {
      if (auto* prompt = restored.findChild<QMessageBox*>("matchEditorDiscardPrompt")) {
        unexpected_prompt = true;
        widget<QPushButton>(*prompt, "matchEditorDiscardChanges")->click();
      }
    });
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&restored, &escape);
    QApplication::processEvents();
    require(
        !unexpected_prompt && !restored.isVisible(), "Unchanged matches must close without confirmation after undo");
    automatic.frames[0].images[0] = root.path().toStdString() + "/missing.png";
    MatchEditorDialog missing(automatic, automatic);
    require(
        !widget<QPushButton>(missing, "matchEditorSave")->isEnabled(), "Missing images must disable editing and save");
    std::cout << "Match editor checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
