#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

#include <cmath>
#include <functional>
#include <iostream>
#include <memory>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool wait_until(const std::function<bool()>& condition, int timeout_ms = 3000) {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < timeout_ms) {
    QApplication::processEvents();
    QTest::qWait(5);
  }
  return condition();
}

class SelectorServer {
 public:
  explicit SelectorServer(int response_status = 200) : response_status_(response_status) {
    QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this]() {
      QTcpSocket* socket = server_.nextPendingConnection();
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
        request_ += socket->readAll();
        const qsizetype header_end = request_.indexOf("\r\n\r\n");
        if (header_end < 0)
          return;
        const QRegularExpression content_length_pattern(
            "Content-Length: ([0-9]+)", QRegularExpression::CaseInsensitiveOption);
        const auto match = content_length_pattern.match(QString::fromLatin1(request_.left(header_end)));
        const int content_length = match.hasMatch() ? match.captured(1).toInt() : 0;
        if (request_.size() < header_end + 4 + content_length)
          return;
        request_received_ = true;
        if (response_status_ == 0)
          return;
        const QByteArray body = response_status_ == 200 ? "saved\n" : "invalid test selection\n";
        const QByteArray reason = response_status_ == 200 ? "OK" : "Bad Request";
        socket->write(
            "HTTP/1.1 " + QByteArray::number(response_status_) + " " + reason + "\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" + body);
        socket->flush();
        socket->disconnectFromHost();
        complete_ = true;
      });
    });
  }

  bool listen() {
    return server_.listen(QHostAddress::LocalHost, 0);
  }

  QUrl url() const {
    return QUrl(QString("http://127.0.0.1:%1/?token=%2").arg(server_.serverPort()).arg(QString(64, 'a')));
  }

  bool complete() const {
    return complete_;
  }

  bool requestReceived() const {
    return request_received_;
  }

  QByteArray request() const {
    return request_;
  }

 private:
  QTcpServer server_;
  int response_status_;
  QByteArray request_;
  bool request_received_{false};
  bool complete_{false};
};

bool test_canvas_controls(const QString& image_path) {
  ScoreboardSelectionCanvas canvas;
  canvas.resize(500, 400);
  canvas.show();
  QApplication::processEvents();
  bool ok = expect(canvas.setImage(image_path), "canvas must load s.png");
  canvas.setPoints({QPoint(-5, -9), QPoint(80, 10), QPoint(90, 70), QPoint(10, 80), QPoint(50, 50)});
  ok &= expect(canvas.points().size() == 4, "canvas must keep exactly four initial points");
  ok &= expect(canvas.points()[0] == QPoint(0, 0), "canvas must clamp initial points to image bounds");
  canvas.fitImage();
  const double fit_scale = canvas.viewScale();
  canvas.zoomBy(1.25);
  ok &= expect(canvas.viewScale() > fit_scale, "zoom in must increase the canvas scale");
  canvas.actualSize();
  ok &= expect(std::abs(canvas.viewScale() - 1.0) < 0.001, "100% Zoom must restore actual image size");
  canvas.focusPoints();
  ok &= expect(canvas.viewScale() > 0.0, "Focus Points must retain a valid scale");
  canvas.undoLastPoint();
  ok &= expect(canvas.points().size() == 3, "Undo Last Point must remove one point");
  canvas.clearPoints();
  ok &= expect(canvas.points().isEmpty(), "Clear Points must remove the selection");
  canvas.actualSize();
  QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(210, 170));
  ok &= expect(
      canvas.points() == QVector<QPoint>{QPoint(10, 10)}, "clicking the image must add an image-coordinate point");
  QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(210, 170));
  QTest::mouseMove(&canvas, QPoint(220, 180));
  QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(220, 180));
  ok &= expect(canvas.points() == QVector<QPoint>{QPoint(20, 20)}, "dragging a red point must update its coordinates");
  canvas.clearPoints();
  QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(210, 170));
  QTest::mouseMove(&canvas, QPoint(230, 190));
  QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(230, 190));
  QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(230, 190));
  ok &= expect(
      canvas.points() == QVector<QPoint>{QPoint(10, 10)},
      "dragging empty image space must pan without creating or shifting a point");
  return ok;
}

bool test_large_image_viewport_cache(const QString& image_path, const QSize& source_size) {
  ScoreboardSelectionCanvas canvas;
  canvas.resize(901, 521);
  canvas.show();
  if (!expect(canvas.setImage(image_path), "large panorama must load"))
    return false;
  bool ok = expect(canvas.imageSize() == source_size, "large panorama must retain its original coordinate size");
  ok &= expect(
      canvas.previewSize().width() < source_size.width() && canvas.previewSize().height() < source_size.height(),
      "large panorama must decode to a bounded display preview");
  ok &= expect(
      canvas.previewSize().width() <= 8192 &&
          static_cast<qint64>(canvas.previewSize().width()) * canvas.previewSize().height() * 4 <=
              96LL * 1024LL * 1024LL,
      "large panorama preview must stay below its dimension and allocation bounds");
  canvas.setPoints({QPoint(source_size.width() + 50, source_size.height() + 50)});
  ok &= expect(
      canvas.points() == QVector<QPoint>{QPoint(source_size.width() - 1, source_size.height() - 1)},
      "large panorama points must clamp to source coordinates rather than preview coordinates");
  canvas.clearPoints();
  canvas.fitImage();
  const double expected_fit_scale = std::min(
      static_cast<double>(canvas.width()) / source_size.width(),
      static_cast<double>(canvas.height()) / source_size.height());
  ok &= expect(
      std::abs(canvas.viewScale() - expected_fit_scale) < 0.0001,
      "large panorama fit must use the source dimensions rather than preview dimensions");
  QApplication::processEvents();
  const quint64 initial_renders = canvas.viewportRenderCount();
  QElapsedTimer hover_timer;
  hover_timer.start();
  for (int index = 0; index < 200; ++index)
    QTest::mouseMove(&canvas, QPoint(10 + index % 800, 10 + index % 450));
  QApplication::processEvents();
  ok &= expect(
      canvas.viewportRenderCount() == initial_renders,
      "hovering a large panorama must reuse the cached viewport instead of rescaling the image");
  ok &= expect(hover_timer.elapsed() < 2000, "cached large-panorama hover updates must remain responsive");
  QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(300, 220));
  QApplication::processEvents();
  ok &= expect(
      canvas.viewportRenderCount() == initial_renders,
      "adding or painting selection points must not rescale the large panorama");
  canvas.zoomBy(1.25);
  QApplication::processEvents();
  ok &= expect(
      canvas.viewportRenderCount() == initial_renders + 1,
      "zooming must invalidate and rebuild the large-panorama viewport exactly once");
  const quint64 zoom_renders = canvas.viewportRenderCount();
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  QEvent dpr_change(QEvent::DevicePixelRatioChange);
  QApplication::sendEvent(&canvas, &dpr_change);
  QApplication::processEvents();
  ok &= expect(
      canvas.viewportRenderCount() == zoom_renders + 1,
      "a device-pixel-ratio change must rebuild the panorama viewport at the new display resolution");
#else
  (void)zoom_renders;
#endif
  return ok;
}

bool test_absurd_source_dimensions_are_rejected(const QString& image_path) {
  ScoreboardSelectionCanvas canvas;
  return expect(!canvas.setImage(image_path), "absurd source dimensions must be rejected before PNG decode");
}

bool test_successful_submission(const QString& image_path) {
  SelectorServer server;
  if (!expect(server.listen(), "test selector server must listen"))
    return false;
  auto dialog = std::make_unique<ScoreboardSelectionDialog>(
      server.url(), image_path, QVector<QPoint>{QPoint(10, 10), QPoint(90, 10), QPoint(90, 70), QPoint(10, 70)});
  dialog->setAttribute(Qt::WA_DeleteOnClose, false);
  QSignalSpy accepted(dialog.get(), &QDialog::accepted);
  dialog->show();
  QApplication::processEvents();
  QPushButton* save = dialog->findChild<QPushButton*>("scoreboardSaveButton");
  const QStringList documented_buttons = {
      "scoreboardZoomOutButton",
      "scoreboardZoomInButton",
      "scoreboardFitButton",
      "scoreboardActualSizeButton",
      "scoreboardFocusButton",
      "scoreboardUndoButton",
      "scoreboardClearButton",
      "scoreboardNoScoreboardButton",
      "scoreboardCancelButton",
      "scoreboardSaveButton",
  };
  bool ok = true;
  for (const QString& object_name : documented_buttons) {
    auto* button = dialog->findChild<QPushButton*>(object_name);
    const QByteArray message =
        QString("Scoreboard action should provide detailed hover help: %1").arg(object_name).toUtf8();
    ok &= expect(
        button && button->toolTip().trimmed().size() >= 20 && button->statusTip() == button->toolTip(),
        message.constData());
  }
  ok &= expect(save && save->isEnabled(), "Save Selection must enable when four points exist");
  if (save)
    save->click();
  ok &= expect(
      wait_until([&]() { return server.complete() && accepted.count() == 1; }),
      "successful backend response must accept the dialog");
  const QByteArray request = server.request();
  ok &= expect(request.startsWith("POST /save?token="), "Save Selection must call the private /save endpoint");
  ok &= expect(request.contains("Origin: http://127.0.0.1:"), "Save Selection must send the required origin");
  ok &= expect(
      request.contains("\"points\":[[10,10],[90,10],[90,70],[10,70]]"),
      "Save Selection must submit all four image coordinates");
  return ok;
}

bool test_failed_submission_remains_open(const QString& image_path) {
  SelectorServer server(400);
  if (!expect(server.listen(), "failure test selector server must listen"))
    return false;
  ScoreboardSelectionDialog dialog(
      server.url(), image_path, {QPoint(10, 10), QPoint(90, 10), QPoint(90, 70), QPoint(10, 70)});
  dialog.setAttribute(Qt::WA_DeleteOnClose, false);
  dialog.show();
  QApplication::processEvents();
  QPushButton* save = dialog.findChild<QPushButton*>("scoreboardSaveButton");
  if (save)
    save->click();
  QLabel* status = dialog.findChild<QLabel*>("scoreboardStatusMessage");
  bool ok = expect(
      wait_until([&]() { return server.complete() && save && save->isEnabled(); }),
      "failed backend response must re-enable the selector");
  ok &= expect(dialog.isVisible(), "failed backend response must keep the selector open");
  ok &= expect(
      status && status->text().contains("invalid test selection"),
      "failed backend response must show the backend error");
  dialog.closeAfterBackendCompletion();
  return ok;
}

bool test_cancel_success(const QString& image_path) {
  SelectorServer server;
  if (!expect(server.listen(), "cancel test selector server must listen"))
    return false;
  ScoreboardSelectionDialog dialog(server.url(), image_path, {});
  dialog.setAttribute(Qt::WA_DeleteOnClose, false);
  QSignalSpy rejected(&dialog, &QDialog::rejected);
  dialog.show();
  QApplication::processEvents();
  QPushButton* cancel = dialog.findChild<QPushButton*>("scoreboardCancelButton");
  if (cancel)
    cancel->click();
  bool ok = expect(
      wait_until([&]() { return server.complete() && rejected.count() == 1; }),
      "a successful /cancel response must close the dialog");
  ok &= expect(server.request().startsWith("POST /cancel?token="), "Cancel must call the private /cancel endpoint");
  return ok;
}

bool test_cancel_failure_escapes_modal(const QString& image_path, bool timeout) {
  SelectorServer server(timeout ? 0 : 400);
  if (!expect(server.listen(), "cancel failure selector server must listen"))
    return false;
  ScoreboardSelectionDialog dialog(server.url(), image_path, {}, nullptr, 100);
  dialog.setAttribute(Qt::WA_DeleteOnClose, false);
  QSignalSpy rejected(&dialog, &QDialog::rejected);
  bool cancellation_failed = false;
  QString cancellation_error;
  dialog.cancellationFailed = [&](const QString& error) {
    cancellation_failed = true;
    cancellation_error = error;
  };
  dialog.show();
  QApplication::processEvents();
  QPushButton* cancel = dialog.findChild<QPushButton*>("scoreboardCancelButton");
  if (cancel)
    cancel->click();
  const bool escaped = wait_until([&]() { return cancellation_failed && rejected.count() == 1; });
  bool ok = expect(escaped, "a failed or timed-out /cancel request must escape the application-modal dialog");
  ok &= expect(!dialog.isVisible(), "cancel failure must not leave an uncloseable modal visible");
  ok &= expect(!cancellation_error.isEmpty(), "cancel failure must report why the owning pipeline should stop");
  ok &= expect(server.requestReceived(), "cancel failure test must reach the backend selector");
  return ok;
}

bool test_cancel_backend_disappears(const QString& image_path) {
  QTcpServer port_reservation;
  if (!port_reservation.listen(QHostAddress::LocalHost, 0))
    return false;
  const quint16 unused_port = port_reservation.serverPort();
  port_reservation.close();
  const QUrl selector_url(QString("http://127.0.0.1:%1/?token=%2").arg(unused_port).arg(QString(64, 'b')));
  ScoreboardSelectionDialog dialog(selector_url, image_path, {}, nullptr, 100);
  dialog.setAttribute(Qt::WA_DeleteOnClose, false);
  QSignalSpy rejected(&dialog, &QDialog::rejected);
  bool cancellation_failed = false;
  dialog.cancellationFailed = [&](const QString&) { cancellation_failed = true; };
  dialog.show();
  QApplication::processEvents();
  if (auto* cancel = dialog.findChild<QPushButton*>("scoreboardCancelButton"))
    cancel->click();
  return expect(
      wait_until([&]() { return cancellation_failed && rejected.count() == 1; }),
      "a disappeared backend must not trap the user in the application-modal dialog");
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir directory;
  if (!directory.isValid())
    return 1;
  const QString image_path = directory.filePath("s.png");
  QImage image(100, 80, QImage::Format_RGB32);
  image.fill(QColor(35, 80, 110));
  if (!image.save(image_path))
    return 1;
  const QString large_image_path = directory.filePath("large-s.png");
  const QSize large_image_size(13931, 4968);
  {
    QImage large_image(large_image_size, QImage::Format_RGB888);
    large_image.fill(QColor(28, 71, 96));
    if (!large_image.save(large_image_path))
      return 1;
  }
  const QString absurd_dimension_path = directory.filePath("absurd-width-s.png");
  {
    QImage absurd_dimension_image(65536, 1, QImage::Format_RGB888);
    absurd_dimension_image.fill(QColor(28, 71, 96));
    if (!absurd_dimension_image.save(absurd_dimension_path))
      return 1;
  }
  const bool ok = test_canvas_controls(image_path) &&
      test_large_image_viewport_cache(large_image_path, large_image_size) &&
      test_absurd_source_dimensions_are_rejected(absurd_dimension_path) && test_successful_submission(image_path) &&
      test_failed_submission_remains_open(image_path) && test_cancel_success(image_path) &&
      test_cancel_failure_escapes_modal(image_path, false) && test_cancel_failure_escapes_modal(image_path, true) &&
      test_cancel_backend_disappears(image_path);
  return ok ? 0 : 1;
}
