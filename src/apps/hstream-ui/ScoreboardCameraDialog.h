#pragma once

#include <yaml-cpp/yaml.h>
#include <QtWidgets/QDialog>
#include <atomic>
#include <thread>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class ScoreboardSelectionCanvas;

class ScoreboardCameraDialog : public QDialog {
 public:
  ScoreboardCameraDialog(const YAML::Node& camera, const QString& game_directory, QWidget* parent = nullptr);
  ~ScoreboardCameraDialog() override;
  YAML::Node selection() const;

 private:
  void refresh();
  void preview();
  void save();
  void sourceChanged();
  YAML::Node camera_;
  QString game_directory_;
  QCheckBox* enabled_;
  QComboBox* mode_;
  QPlainTextEdit* files_;
  QPushButton* browse_;
  QLineEdit* uri_;
  QComboBox* codec_;
  QCheckBox* ntp_;
  QLineEdit* offset_;
  QLineEdit* preview_time_;
  ScoreboardSelectionCanvas* canvas_;
  QLabel* status_;
  QPushButton* preview_;
  QPushButton* save_;
  std::atomic<bool> cancelled_{false};
  std::thread worker_;
  bool busy_{false};
  bool loaded_{false};
  bool crop_changed_{false};
};
