#pragma once

#include <QtWidgets/QDialog>

#include <map>
#include <memory>

// Experiments own their recording, historical state and controls independently
// of the live Program window. Closing this dialog cannot change a live preset.
class CameraExperimentDialog : public QDialog {
 public:
  explicit CameraExperimentDialog(
      const QString& game_directory = {},
      QWidget* parent = nullptr,
      const std::map<QString, double>& camera_controls = {});
  ~CameraExperimentDialog() override;
  bool captureScreenshot(const QString& path, QString* error = nullptr);
  void done(int result) override;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
