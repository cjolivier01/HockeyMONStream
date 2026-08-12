#pragma once

#include <QtCore/QPoint>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtGui/QImage>
#include <QtWidgets/QDialog>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QCloseEvent;
class QMouseEvent;
class QPaintEvent;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QResizeEvent;
class QWheelEvent;

class ScoreboardSelectionCanvas : public QWidget {
 public:
  explicit ScoreboardSelectionCanvas(QWidget* parent = nullptr);

  bool setImage(const QString& path);
  void setPoints(const QVector<QPoint>& points);
  const QVector<QPoint>& points() const;
  double viewScale() const;
  void fitImage();
  void actualSize();
  void focusPoints();
  void zoomBy(double factor);
  void undoLastPoint();
  void clearPoints();

  std::function<void()> selectionChanged;
  std::function<void(const QPoint&, bool)> hoverChanged;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  QPoint clampImagePoint(const QPointF& point) const;
  QPointF imageToScreen(const QPoint& point) const;
  QPointF screenToImage(const QPointF& point) const;
  int pointNear(const QPointF& position) const;
  void setScaleAround(const QPointF& position, double scale);
  void notifySelectionChanged();

  QImage image_;
  QVector<QPoint> points_;
  double view_scale_{1.0};
  QPointF view_offset_;
  bool view_initialized_{false};
  bool pointer_active_{false};
  bool pointer_moved_{false};
  int dragged_point_{-1};
  QPointF press_position_;
  QPointF pan_start_offset_;
  QPoint hover_point_;
  bool hover_valid_{false};
};

class ScoreboardSelectionDialog : public QDialog {
 public:
  ScoreboardSelectionDialog(
      const QUrl& selector_url,
      const QString& image_path,
      const QVector<QPoint>& initial_points,
      QWidget* parent = nullptr);

  QString loadError() const;
  void closeAfterBackendCompletion();

 protected:
  void reject() override;
  void closeEvent(QCloseEvent* event) override;

 private:
  enum class Submission {
    kSave,
    kNoScoreboard,
    kCancel,
  };

  void buildUi();
  void refreshSelectionUi();
  void updateHover(const QPoint& point, bool valid);
  void submit(Submission submission);
  void finishSubmission(QNetworkReply* reply, Submission submission);
  void requestCancel();
  QUrl endpointFor(Submission submission) const;

  QUrl selector_url_;
  QString image_path_;
  QVector<QPoint> initial_points_;
  QString load_error_;
  ScoreboardSelectionCanvas* canvas_{nullptr};
  QLabel* selection_count_{nullptr};
  QLabel* status_title_{nullptr};
  QLabel* status_message_{nullptr};
  QLabel* hover_coordinates_{nullptr};
  QVector<QLabel*> point_values_;
  QPushButton* save_button_{nullptr};
  QPushButton* no_scoreboard_button_{nullptr};
  QPushButton* focus_button_{nullptr};
  QPushButton* undo_button_{nullptr};
  QPushButton* clear_button_{nullptr};
  QPushButton* cancel_button_{nullptr};
  QNetworkAccessManager* network_{nullptr};
  QPointer<QNetworkReply> pending_reply_;
  bool submitting_{false};
  bool backend_completed_{false};
};
