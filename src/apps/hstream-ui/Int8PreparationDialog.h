#pragma once

#include <yaml-cpp/yaml.h>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtWidgets/QDialog>
#include <filesystem>

class QLabel;
class QPlainTextEdit;
class QSpinBox;
class QPushButton;
class QCheckBox;

namespace hm::ui {
struct Int8PreparationDefaults {
  int samples;
  bool play_when_ready;
};
Int8PreparationDefaults ReadInt8PreparationDefaults(const YAML::Node& config);

struct Int8PreparationRequest {
  Int8PreparationDefaults defaults;
  QString runner, working_directory, bazel_bin, app_config, detector_config, game_id;
  QProcessEnvironment environment;
  bool hockeymon{false};
  QString detector_model;
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

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void start();
  void readOutput();
  void finishWhenStopped(int code, QProcess::ExitStatus status);
  std::filesystem::path operation_game_;
  std::string operation_token_;
  qint64 retirement_started_ms_{0};
  int poll_interval_ms_{0}, shutdown_timeout_ms_{0};
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
