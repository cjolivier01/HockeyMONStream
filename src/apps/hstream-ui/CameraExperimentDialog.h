#pragma once

#include <QtWidgets/QDialog>

#include <memory>

// Experiments own their recording, historical state and controls independently
// of the live Program window. Closing this dialog cannot change a live preset.
class CameraExperimentDialog : public QDialog {
 public:
  explicit CameraExperimentDialog(const QString& game_directory = {}, QWidget* parent = nullptr);
  ~CameraExperimentDialog() override;
  bool captureScreenshot(const QString& path, QString* error = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
