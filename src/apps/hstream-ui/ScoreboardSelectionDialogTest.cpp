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

  QByteArray request() const {
    return request_;
  }

 private:
  QTcpServer server_;
  int response_status_;
  QByteArray request_;
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
  bool ok = expect(save && save->isEnabled(), "Save Selection must enable when four points exist");
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
  const bool ok = test_canvas_controls(image_path) && test_successful_submission(image_path) &&
      test_failed_submission_remains_open(image_path);
  return ok ? 0 : 1;
}
