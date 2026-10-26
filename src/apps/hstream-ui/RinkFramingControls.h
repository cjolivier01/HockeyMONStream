#pragma once

#include <yaml-cpp/yaml.h>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>
#include <array>
#include <functional>
#include <memory>
#include "absl/status/status.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QThread;

// Next-run choices shared by desktop playback and exported Program jobs.
class RinkFramingControls : public QWidget {
 public:
  using MapCheck = std::function<absl::Status(const QString&, const std::function<bool()>&)>;
  explicit RinkFramingControls(QWidget* parent = nullptr, MapCheck check = {});
  ~RinkFramingControls() override;
  void setGameDirectory(const QString& directory);
  void checkMap(bool force = false);
  void setCheckingSuspended(bool suspended);
  QString validateForRun();
  bool isCheckingForRun();
  QString mapReadinessMessage() const;
  QString validateSavedSettings(const YAML::Node& config) const;
  void loadConfig(const YAML::Node& defaults, const YAML::Node& user, const YAML::Node& game);
  void resetToDefaults();
  bool isDirty() const;
  void applyChanges(YAML::Node& destination) const;
  QStringList arguments() const;
  void setChangedCallback(std::function<void()> callback) {
    changed_ = std::move(callback);
  }
  void setReadinessCallback(std::function<void()> callback) {
    readiness_changed_ = std::move(callback);
  }
  void showRunStatus(const QString& message);

 private:
  void refresh();
  void startMapCheck();
  QByteArray mapSignature() const;
  struct MapJob;
  MapCheck check_;
  QString directory_;
  QByteArray signature_;
  QString map_error_{"Select a game with a saved, reviewed rink map."};
  QFileSystemWatcher watcher_;
  QTimer check_timer_;
  QThread* worker_{nullptr};
  std::shared_ptr<MapJob> job_;
  bool check_pending_{false};
  bool map_ready_{false};
  bool checking_suspended_{false};
  std::array<QCheckBox*, 4> choices_{};
  std::array<bool, 4> defaults_{};
  std::array<bool, 4> loaded_{};
  QLabel* status_{nullptr};
  QPushButton* turn_off_{nullptr};
  std::function<void()> changed_;
  std::function<void()> readiness_changed_;
  bool loading_{false};
};
