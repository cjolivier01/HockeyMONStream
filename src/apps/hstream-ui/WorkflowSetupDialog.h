#pragma once

#include <yaml-cpp/yaml.h>
#include <QtCore/QProcessEnvironment>
#include <QtWidgets/QDialog>
#include <memory>

class WorkflowSetupDialog : public QDialog {
 public:
  WorkflowSetupDialog(
      const QString& launcher,
      const QString& registry,
      const QString& game,
      const QProcessEnvironment& environment,
      const YAML::Node& settings,
      QWidget* parent);
  ~WorkflowSetupDialog() override;
  QString selectedRegistry() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
