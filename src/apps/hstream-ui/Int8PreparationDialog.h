#pragma once

#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtWidgets/QDialog>

class QLabel;
class QPlainTextEdit;
class QSpinBox;
class QPushButton;
class QCheckBox;

namespace hm::ui {
struct Int8PreparationRequest {
  QString runner, working_directory, bazel_bin, app_config, detector_config, game_id;
  QProcessEnvironment environment;
};

class Int8PreparationDialog : public QDialog {
 public:
  explicit Int8PreparationDialog(Int8PreparationRequest request, QWidget* parent = nullptr);
  ~Int8PreparationDialog() override;
  QString engine() const {
    return engine_;
  }
  QString manifest() const {
    return manifest_;
  }
  bool playWhenReady() const;
  void reject() override;

 private:
  void start();
  void readOutput();
  Int8PreparationRequest request_;
  QProcess process_;
  QByteArray output_;
  QString engine_, manifest_;
  QLabel* status_;
  QPlainTextEdit* log_;
  QSpinBox* count_;
  QPushButton* prepare_;
  QCheckBox* play_;
  bool cancelled_{false};
};
} // namespace hm::ui
