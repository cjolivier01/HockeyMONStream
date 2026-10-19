#pragma once

#include <yaml-cpp/yaml.h>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>
#include <array>
#include <functional>

class QCheckBox;
class QLabel;

// Next-run choices shared by desktop playback and exported Program jobs.
class RinkFramingControls : public QWidget {
 public:
  explicit RinkFramingControls(QWidget* parent = nullptr);
  void loadConfig(const YAML::Node& defaults, const YAML::Node& user, const YAML::Node& game);
  void resetToDefaults();
  bool isDirty() const;
  void applyChanges(YAML::Node& destination) const;
  QStringList arguments() const;
  void setChangedCallback(std::function<void()> callback) {
    changed_ = std::move(callback);
  }
  void showRunStatus(const QString& message);

 private:
  void refresh();
  std::array<QCheckBox*, 4> choices_{};
  std::array<bool, 4> loaded_{{false, true, false, false}};
  QLabel* status_{nullptr};
  std::function<void()> changed_;
  bool loading_{false};
};
