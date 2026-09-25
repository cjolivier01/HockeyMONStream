#pragma once

#include <yaml-cpp/yaml.h>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtWidgets/QWidget>

#include <functional>
#include <vector>

class QLabel;

// Next-run preferences only. This widget never emits runtime commands, loads an
// inference engine, or inspects disabled model/config paths. Separate layers
// preserve native-over-canonical precedence without flattening away provenance.
class PlayerAnalyticsControls : public QWidget {
 public:
  explicit PlayerAnalyticsControls(QWidget* parent = nullptr);
  void setChangedCallback(std::function<void()> callback);
  // defaults contains canonical baseline plus structural pipeline under pipeline.
  void loadConfig(
      const YAML::Node& defaults,
      const YAML::Node& user,
      const YAML::Node& game,
      const QString& structural_directory);
  void resetToDefaults();
  bool isDirty() const;
  // Transactionally update only edited leaves, preserving all other settings.
  QString applyChanges(YAML::Node& destination) const;
  // Complete effective UI choices; used by both launch and saved job arguments.
  // Does not access files; callers validate before launching a Program run.
  QStringList arguments() const;
  // Bounded enabled-only manifest/config checks; no engine/ONNX bytes or CUDA.
  QString validateForRun();

 private:
  enum class Kind { kFlag, kPath, kRoi };
  struct Field {
    QString key;
    QString native_alias;
    Kind kind;
    QWidget* editor{nullptr};
    QVariant loaded;
    bool valid{true};
    bool touched{false};
  };
  QVariant value(const Field& field) const;
  QVariant read(const Field& field, const std::vector<YAML::Node>& layers, bool* valid) const;
  void setValue(Field& field, const QVariant& value);
  bool changed(const Field& field) const;
  void changed(size_t index);
  YAML::Node effectiveConfig() const;
  QString validation(bool inspect_files) const;
  void updateStatus();
  void addFlag(const QString& key, const QString& alias, const QString& name, const QString& label, QWidget* parent);
  void addPath(const QString& key, const QString& name, bool directory, QWidget* parent);
  std::vector<Field> fields_;
  YAML::Node defaults_;
  YAML::Node user_;
  YAML::Node game_;
  QString structural_directory_;
  QLabel* status_{nullptr};
  std::function<void()> changed_callback_;
  bool loading_{false};
};
