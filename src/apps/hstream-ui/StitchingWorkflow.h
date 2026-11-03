#pragma once

#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include <yaml-cpp/yaml.h>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>
#include <QtWidgets/QWidget>

#include <functional>
#include <memory>

// Compiles only immutable, already-prepared catalog rows. CPU metadata stages
// use the exact installed admission profile; GPU stages use the chosen profile.
absl::StatusOr<QJsonObject> BuildStitchingWorkflowPlan(
    const StitchingExperimentStore& store,
    const StitchingExperimentCatalog& catalog,
    const QJsonObject& published_profiles,
    const QString& profile_id,
    int max_parallel,
    const QString& workflow_id);

class StitchingWorkflow : public QWidget {
 public:
  StitchingWorkflow(
      const QString& game,
      const QString& working_directory,
      const QProcessEnvironment& environment,
      const YAML::Node& settings,
      QWidget* parent);
  ~StitchingWorkflow() override;
  bool selected() const;
  bool active() const;
  bool busy() const;
  bool hasSelection() const;
  QString workflowId() const;
  void setEditingEnabled(bool enabled, bool queued);
  void start(const StitchingExperimentStore& store, const StitchingExperimentCatalog& catalog);
  void exportBatch(const StitchingExperimentStore& store, const StitchingExperimentCatalog& catalog);
  void cancel();
  void attach(const QString& workflow_id);
  void selectCandidate(const QString& catalog_key);
  void showLog();
  std::function<void()> changed;
  std::function<void(const QJsonObject&)> snapshotChanged;
  std::function<void(const QString&, bool)> message;
  std::function<void()> exportRequested;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
