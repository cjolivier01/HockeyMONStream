#pragma once

#include <QtCore/QProcessEnvironment>
#include <QtWidgets/QDialog>

#include <functional>
#include <memory>

class StitchingExperimentDialog : public QDialog {
 public:
  explicit StitchingExperimentDialog(
      const QString& game_directory,
      const QString& runner,
      const QString& working_directory,
      const QString& pipeline_config,
      const QProcessEnvironment& environment,
      int control_points,
      int frame_count,
      const QString& stitch_frame_time,
      QWidget* parent = nullptr,
      std::function<void()> selection_applied = {});
  ~StitchingExperimentDialog() override;
  void done(int result) override;

 protected:
  void closeEvent(QCloseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
