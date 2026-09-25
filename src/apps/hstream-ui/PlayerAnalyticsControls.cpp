#include "src/apps/hstream-ui/PlayerAnalyticsControls.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <stdexcept>

#include "hstream/src/libs/player_analytics/Config.h"
#include "hstream/src/libs/player_analytics/ModelCatalog.h"
#include "hstream/src/libs/player_analytics/ModelContract.h"

namespace {
namespace pa = hm::player_analytics;
const QString kAnalytics = "pipeline.player-analytics.";
QString ModelKey(const QString& path) {
  return path == "pipeline.tracker.reid-config-file" ? QString("pipeline.tracker.reid-model")
                                                     : path.left(path.lastIndexOf('.')) + ".model";
}
QString CustomPathKey(const QString& model) {
  return model == "pipeline.tracker.reid-model" ? QString("pipeline.tracker.reid-config-file")
                                                : model.left(model.lastIndexOf('.')) + ".bundle";
}
std::string ModelFeature(const QString& key) {
  return key == "pipeline.tracker.reid-model" ? "reid" : key.section('.', -2, -2).toStdString();
}
YAML::Node Get(const YAML::Node& root, const QString& path, bool* ancestor_replaced = nullptr) {
  if (ancestor_replaced)
    *ancestor_replaced = false;
  YAML::Node current(root);
  for (const auto& key : path.split('.')) {
    if (!current.IsDefined() || !current.IsMap()) {
      if (ancestor_replaced)
        *ancestor_replaced = current.IsDefined();
      return YAML::Node(YAML::NodeType::Undefined);
    }
    const YAML::Node next = static_cast<const YAML::Node&>(current)[key.toStdString()];
    if (!next.IsDefined())
      return YAML::Node(YAML::NodeType::Undefined);
    current.reset(next);
  }
  return current;
}
YAML::Node Merge(const YAML::Node& low, const YAML::Node& high) {
  if (!high.IsDefined())
    return YAML::Clone(low);
  if (!low.IsDefined() || !low.IsMap() || !high.IsMap())
    return YAML::Clone(high);
  YAML::Node result = YAML::Clone(low);
  for (const auto& item : high) {
    const auto key = item.first.as<std::string>();
    result[key] = Merge(static_cast<const YAML::Node&>(result)[key], item.second);
  }
  return result;
}
void Set(YAML::Node node, const QStringList& path, const YAML::Node& value, int index = 0) {
  if (!node.IsDefined() || node.IsNull())
    node = YAML::Node(YAML::NodeType::Map);
  if (!node.IsMap())
    throw std::invalid_argument("Cannot edit player settings beneath a non-mapping configuration key");
  if (index + 1 == path.size())
    node[path[index].toStdString()] = YAML::Clone(value);
  else
    Set(node[path[index].toStdString()], path, value, index + 1);
}
bool Flag(const YAML::Node& node, bool fallback = false) {
  if (!node.IsDefined() || node.IsNull())
    return fallback;
  if (!node.IsScalar())
    throw std::invalid_argument("Expected a boolean or 0/1");
  if (node.Scalar() == "1")
    return true;
  if (node.Scalar() == "0")
    return false;
  return node.as<bool>();
}
bool ProgramBoxes(const std::vector<YAML::Node>& layers) {
  struct Ranked {
    YAML::Node value{YAML::NodeType::Undefined};
    int rank{-1};
  };
  const auto read = [&layers](const QString& path) {
    Ranked result;
    for (size_t rank = 0; rank < layers.size(); ++rank) {
      bool replaced = false;
      const auto value = Get(layers[rank], path, &replaced);
      if (value.IsDefined() || replaced) {
        result.value.reset(value);
        result.rank = static_cast<int>(rank);
      }
    }
    return result;
  };
  const auto debug = read("plot.debug_play_tracker");
  const auto individual = read("plot.plot_individual_player_tracking");
  const auto native = read("pipeline.hmplaycropper.plot-player-tracking");
  const int source_rank = std::max({0, debug.rank, individual.rank});
  // Match Configurator's map_bool_or: a true source carries its own rank,
  // rather than the rank of a later false source. Structural native defaults
  // also win when neither canonical setting has an explicit override.
  if (native.value.IsDefined() && !native.value.IsNull() && native.rank >= source_rank)
    return Flag(native.value);
  const auto canonical_flag = [](const YAML::Node& node) {
    if (node.IsDefined() && node.IsNull())
      throw std::invalid_argument("Program box drawing requires non-null canonical booleans");
    return Flag(node);
  };
  const bool debug_on = canonical_flag(debug.value), individual_on = canonical_flag(individual.value);
  const int mapped_rank = debug_on || individual_on
      ? std::max(debug_on ? std::max(0, debug.rank) : -1, individual_on ? std::max(0, individual.rank) : -1)
      : source_rank;
  if (native.value.IsDefined() && !native.value.IsNull() && native.rank >= mapped_rank)
    return Flag(native.value);
  return debug_on || individual_on;
}
QString Absolute(const QString& path, const QString& directory) {
  return QDir::cleanPath(QFileInfo(path).isAbsolute() ? path : QDir(directory).absoluteFilePath(path));
}
bool ReadableFile(const QString& path) {
  const QFileInfo info(path);
  return info.isFile() && info.isReadable() && info.size() > 0;
}
YAML::Node Document(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 1024 * 1024)
    throw std::invalid_argument(("Cannot read bounded model/config metadata: " + path).toStdString());
  return YAML::Load(file.readAll().toStdString());
}
} // namespace

PlayerAnalyticsControls::PlayerAnalyticsControls(QWidget* parent) : QWidget(parent) {
  setObjectName("playerAnalyticsControls");
  auto* layout = new QVBoxLayout(this);
  auto* explanation = new QLabel(
      "Player analytics (next run). Selected models are downloaded and prepared automatically when needed, "
      "then reused. The first run may take longer. Drawing does not enable a model.");
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto* compute = new QGroupBox("Compute");
  new QFormLayout(compute);
  addFlag(kAnalytics + "pose.enable", {}, "playerPoseEnable", "Estimate pose", compute);
  addModel(kAnalytics + "pose.model", "playerPoseModel", "Pose model", compute);
  addPath(kAnalytics + "pose.bundle", "playerPoseBundle", true, compute);
  addFlag(kAnalytics + "jersey.enable", {}, "playerJerseyEnable", "Read jersey numbers", compute);
  addModel(kAnalytics + "jersey.model", "playerJerseyModel", "Jersey model", compute);
  addPath(kAnalytics + "jersey.bundle", "playerJerseyBundle", true, compute);
  auto* roi = new QComboBox();
  roi->setObjectName("playerJerseyRoiMode");
  roi->addItem("Bounding-box torso", "bbox");
  roi->addItem("Fresh pose torso (requires pose)", "pose");
  static_cast<QFormLayout*>(compute->layout())->addRow("Jersey crop", roi);
  fields_.push_back({kAnalytics + "jersey.roi-mode", {}, Kind::kRoi, roi, "bbox"});
  const size_t roi_index = fields_.size() - 1;
  connect(roi, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, roi_index] { changed(roi_index); });
  addFlag(kAnalytics + "action.enable", {}, "playerActionEnable", "Recognize activities", compute);
  addModel(kAnalytics + "action.model", "playerActionModel", "Activity model", compute);
  addPath(kAnalytics + "action.bundle", "playerActionBundle", true, compute);
  auto* action_help = new QLabel(
      "Activities require pose at 10 Hz or faster and 9.9 seconds of continuous observations. "
      "Labels describe generic activities, not hockey events.");
  action_help->setWordWrap(true);
  static_cast<QFormLayout*>(compute->layout())->addRow(action_help);
  layout->addWidget(compute);
  auto* tracker = new QGroupBox("Native tracker appearance matching");
  new QFormLayout(tracker);
  addFlag("pipeline.tracker.reid-enable", {}, "playerReidEnable", "Improve player appearance matching", tracker);
  addModel("pipeline.tracker.reid-model", "playerReidModel", "Appearance model", tracker);
  addPath("pipeline.tracker.reid-config-file", "playerReidConfig", false, tracker);
  auto* tracker_help =
      new QLabel("Off preserves the existing tracker configuration, including any ReID it already uses.");
  tracker_help->setWordWrap(true);
  static_cast<QFormLayout*>(tracker->layout())->addRow(tracker_help);
  layout->addWidget(tracker);
  auto* drawing = new QGroupBox("Drawing");
  new QFormLayout(drawing);
  addFlag("plot.plot_pose", kAnalytics + "draw-pose", "playerDrawPose", "Pose skeletons", drawing);
  addFlag("plot.plot_jersey_numbers", kAnalytics + "draw-jerseys", "playerDrawJerseys", "Jersey numbers", drawing);
  addFlag("plot.plot_actions", kAnalytics + "draw-actions", "playerDrawActions", "Activity labels", drawing);
  addFlag(
      "plot.plot_individual_player_tracking",
      "pipeline.hmplaycropper.plot-player-tracking",
      "playerDrawBoxes",
      "Program player boxes",
      drawing);
  layout->addWidget(drawing);
  status_ = new QLabel();
  status_->setObjectName("playerAnalyticsStatus");
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(status_);
  layout->addStretch();
  loadConfig(YAML::Node(), YAML::Node(), YAML::Node(), QDir::currentPath());
}

void PlayerAnalyticsControls::addFlag(
    const QString& key,
    const QString& alias,
    const QString& name,
    const QString& label,
    QWidget* parent) {
  auto* editor = new QCheckBox(label);
  editor->setObjectName(name);
  editor->setToolTip("Applies on the next run. Save Preset keeps this choice for the selected game.");
  static_cast<QFormLayout*>(parent->layout())->addRow(editor);
  fields_.push_back({key, alias, Kind::kFlag, editor, false});
  const size_t index = fields_.size() - 1;
  connect(editor, &QCheckBox::toggled, this, [this, index] { changed(index); });
}
void PlayerAnalyticsControls::addPath(const QString& key, const QString& name, bool directory, QWidget* parent) {
  auto* row = new QWidget();
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* editor = new QLineEdit();
  editor->setObjectName(name);
  editor->setPlaceholderText(directory ? "Prepared custom model folder" : "Custom tracker configuration file");
  editor->setToolTip("Used only with Custom. Relative paths use the pipeline configuration directory.");
  auto* browse = new QPushButton("Browse…");
  browse->setObjectName(name + "Browse");
  layout->addWidget(editor, 1);
  layout->addWidget(browse);
  auto* form = static_cast<QFormLayout*>(parent->layout());
  form->addRow(directory ? "Custom model files" : "Custom configuration", row);
  fields_.push_back({key, {}, Kind::kPath, editor, QString()});
  fields_.back().row = row;
  fields_.back().label = form->labelForField(row);
  const size_t index = fields_.size() - 1;
  connect(editor, &QLineEdit::textChanged, this, [this, index] { changed(index); });
  connect(browse, &QPushButton::clicked, this, [this, editor, directory] {
    const QString initial =
        editor->text().isEmpty() ? structural_directory_ : Absolute(editor->text(), structural_directory_);
    const QString path = directory
        ? QFileDialog::getExistingDirectory(this, "Custom model files", initial)
        : QFileDialog::getOpenFileName(
              this, "Custom tracker configuration", initial, "YAML (*.yaml *.yml);;All files (*)");
    if (!path.isEmpty())
      editor->setText(path);
  });
}
void PlayerAnalyticsControls::addModel(const QString& key, const QString& name, const QString& label, QWidget* parent) {
  auto* editor = new QComboBox();
  editor->setObjectName(name);
  const auto feature = ModelFeature(key);
  const auto id = std::string(pa::DefaultModelId(feature));
  const auto* model = pa::FindModel(feature, id);
  const QString title = model ? QString::fromUtf8(model->label) : QString::fromStdString(id);
  editor->addItem(title, QString::fromStdString(id));
  if (feature == "reid") {
    const std::string alternative = "reidentificationnet-deployable-v1.2";
    const auto* nvidia = pa::FindModel(feature, alternative);
    if (nvidia && alternative != id)
      editor->addItem(QString::fromUtf8(nvidia->label), QString::fromStdString(alternative));
  }
  editor->addItem("Custom…", "custom");
  editor->setProperty("knownModelCount", editor->count());
  editor->setToolTip("Uses the supplied model or downloads it when needed, prepares it for this GPU, then reuses it.");
  static_cast<QFormLayout*>(parent->layout())->addRow(label, editor);
  fields_.push_back({key, {}, Kind::kModel, editor, QString::fromStdString(id)});
  const size_t index = fields_.size() - 1;
  connect(editor, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, index] { changed(index); });
}
void PlayerAnalyticsControls::setChangedCallback(std::function<void()> callback) {
  changed_callback_ = std::move(callback);
}
QVariant PlayerAnalyticsControls::value(const Field& field) const {
  if (field.kind == Kind::kFlag)
    return static_cast<QCheckBox*>(field.editor)->isChecked();
  if (field.kind == Kind::kRoi || field.kind == Kind::kModel)
    return static_cast<QComboBox*>(field.editor)->currentData();
  return static_cast<QLineEdit*>(field.editor)->text();
}
QVariant PlayerAnalyticsControls::read(const Field& field, const std::vector<YAML::Node>& layers, bool* valid) const {
  if (field.key == "plot.plot_individual_player_tracking") {
    try {
      *valid = true;
      return ProgramBoxes(layers);
    } catch (const std::exception&) {
      *valid = false;
      return false;
    }
  }
  YAML::Node resolved(YAML::NodeType::Undefined);
  if (field.native_alias.isEmpty()) {
    YAML::Node merged(YAML::NodeType::Map);
    for (const auto& layer : layers)
      merged = Merge(merged, layer);
    resolved.reset(Get(merged, field.key));
  } else {
    YAML::Node canonical(YAML::NodeType::Undefined), native(YAML::NodeType::Undefined);
    int canonical_rank = -1, native_rank = -1;
    for (size_t rank = 0; rank < layers.size(); ++rank) {
      const auto update = [&](const QString& path, YAML::Node& previous, int& previous_rank) {
        bool replaced = false;
        const auto next = Get(layers[rank], path, &replaced);
        if (next.IsDefined() || replaced) {
          previous.reset(next);
          previous_rank = static_cast<int>(rank);
        }
      };
      update(field.key, canonical, canonical_rank);
      update(field.native_alias, native, native_rank);
    }
    // A null canonical value suppresses its mapping. Same-layer native wins;
    // later canonical values override older native values that still exist.
    if (native.IsDefined() && (!canonical.IsDefined() || canonical.IsNull() || native_rank >= canonical_rank))
      resolved.reset(native);
    else if (canonical.IsDefined() && !canonical.IsNull())
      resolved.reset(canonical);
  }
  *valid = true;
  try {
    if (field.kind == Kind::kFlag)
      return Flag(resolved);
    if (field.kind == Kind::kModel && (!resolved.IsDefined() || resolved.IsNull())) {
      YAML::Node merged(YAML::NodeType::Map);
      for (const auto& layer : layers)
        merged = Merge(merged, layer);
      const auto path = Get(merged, CustomPathKey(field.key));
      if (path.IsDefined() && !path.IsNull() && !path.IsScalar())
        throw std::invalid_argument("Expected a scalar custom model path");
      if (path.IsDefined() && path.IsScalar() && !path.Scalar().empty())
        return QString("custom");
      return QString::fromStdString(std::string(pa::DefaultModelId(ModelFeature(field.key))));
    }
    if (!resolved.IsDefined() || resolved.IsNull())
      return field.kind == Kind::kRoi ? QString("bbox") : QString();
    if (!resolved.IsScalar())
      throw std::invalid_argument("Expected a scalar path/mode");
    return QString::fromStdString(resolved.as<std::string>());
  } catch (const std::exception&) {
    *valid = false;
    return field.kind == Kind::kFlag ? QVariant(false) : QVariant(QString());
  }
}
void PlayerAnalyticsControls::setValue(Field& field, const QVariant& selected) {
  const QSignalBlocker blocker(field.editor);
  if (field.kind == Kind::kFlag)
    static_cast<QCheckBox*>(field.editor)->setChecked(selected.toBool());
  else if (field.kind == Kind::kPath)
    static_cast<QLineEdit*>(field.editor)->setText(selected.toString());
  else {
    auto* combo = static_cast<QComboBox*>(field.editor);
    if (field.kind == Kind::kModel)
      while (combo->count() > combo->property("knownModelCount").toInt())
        combo->removeItem(combo->count() - 1);
    int index = combo->findData(selected);
    if (index < 0) {
      combo->addItem(
          (field.kind == Kind::kModel ? "Unrecognized model: " : "Saved mode: ") + selected.toString(), selected);
      index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
  }
}
void PlayerAnalyticsControls::loadConfig(
    const YAML::Node& defaults,
    const YAML::Node& user,
    const YAML::Node& game,
    const QString& directory) {
  loading_ = true;
  const auto layer = [](const YAML::Node& node) {
    return !node.IsDefined() || node.IsNull() ? YAML::Node(YAML::NodeType::Map) : YAML::Clone(node);
  };
  defaults_ = layer(defaults);
  user_ = layer(user);
  game_ = layer(game);
  structural_directory_ = directory;
  for (auto& field : fields_) {
    field.loaded = read(field, {defaults_, user_, game_}, &field.valid);
    field.touched = false;
    setValue(field, field.loaded);
  }
  loading_ = false;
  updateStatus();
}
void PlayerAnalyticsControls::resetToDefaults() {
  for (auto& field : fields_) {
    bool valid = true;
    setValue(field, read(field, {defaults_, user_}, &valid));
    field.touched = true;
  }
  updateStatus();
  if (changed_callback_)
    changed_callback_();
}
bool PlayerAnalyticsControls::changed(const Field& field) const {
  if (value(field) != field.loaded || (!field.valid && field.touched))
    return true;
  if (field.kind != Kind::kModel)
    return false;
  const auto configured = Get(Merge(Merge(defaults_, user_), game_), field.key);
  if (configured.IsDefined() && !configured.IsNull())
    return false;
  // A path edit can change an implicit selection even when the combo returns
  // to its loaded value. Persist that displayed choice explicitly so saving
  // a retained custom path cannot turn a built-in back into Custom on reload.
  for (const auto& path : fields_) {
    if (path.key != CustomPathKey(field.key) || !changed(path))
      continue;
    const auto inferred = value(path).toString().isEmpty()
        ? QString::fromStdString(std::string(pa::DefaultModelId(ModelFeature(field.key))))
        : QString("custom");
    return value(field).toString() != inferred;
  }
  return false;
}
bool PlayerAnalyticsControls::isDirty() const {
  for (const auto& field : fields_)
    if (changed(field))
      return true;
  return false;
}
void PlayerAnalyticsControls::changed(size_t index) {
  if (loading_)
    return;
  fields_.at(index).touched = true;
  updateStatus();
  if (changed_callback_)
    changed_callback_();
}
QString PlayerAnalyticsControls::applyChanges(YAML::Node& destination) const {
  try {
    YAML::Node staged = YAML::Clone(destination);
    if (!staged.IsDefined() || staged.IsNull())
      staged = YAML::Node(YAML::NodeType::Map);
    for (const auto& field : fields_) {
      if (!changed(field))
        continue;
      const YAML::Node selected = field.kind == Kind::kFlag ? YAML::Node(value(field).toBool())
                                                            : YAML::Node(value(field).toString().toStdString());
      Set(staged, field.key.split('.'), selected);
      // Preserve explicit native settings on unrelated saves. On an edit only,
      // reconcile the conflicting same-destination leaf with the canonical choice.
      if (!field.native_alias.isEmpty() &&
          (Get(staged, field.native_alias).IsDefined() || field.key == "plot.plot_individual_player_tracking"))
        Set(staged, field.native_alias.split('.'), selected);
    }
    destination = staged;
    return {};
  } catch (const std::exception& error) {
    return QString::fromUtf8(error.what());
  }
}
YAML::Node PlayerAnalyticsControls::effectiveConfig() const {
  YAML::Node edited = YAML::Clone(game_);
  const auto error = applyChanges(edited);
  if (!error.isEmpty())
    throw std::invalid_argument(error.toStdString());
  YAML::Node effective = Merge(Merge(defaults_, user_), edited);
  for (const auto& field : fields_)
    if (!field.native_alias.isEmpty())
      Set(effective, field.native_alias.split('.'), YAML::Node(value(field).toBool()));
  return effective;
}
QStringList PlayerAnalyticsControls::arguments() const {
  QStringList result;
  const auto append = [&result](const QString& key, const QString& text) { result << "--options=" + key + "=" + text; };
  for (const auto& field : fields_) {
    // Unedited boxes retain the runner's debug-OR mapping and any advanced
    // cropper private-property precedence. Never synthesize an off override.
    if (field.key == "plot.plot_individual_player_tracking" && !changed(field))
      continue;
    if (field.kind == Kind::kFlag) {
      const QString text = value(field).toBool() ? "true" : "false";
      append(field.key, text);
      if (!field.native_alias.isEmpty())
        append(field.native_alias, text);
    } else {
      const QString owner = field.key.startsWith("pipeline.tracker.")
          ? "pipeline.tracker.reid-enable"
          : field.key.left(field.key.lastIndexOf('.')) + ".enable";
      bool enabled = false;
      for (const auto& candidate : fields_)
        if (candidate.key == owner)
          enabled = value(candidate).toBool();
      if (!enabled)
        continue;
      if (field.kind == Kind::kPath && selectedModel(ModelKey(field.key)) != "custom")
        continue;
      const QString text = value(field).toString();
      // Saved paths remain in their original layer. The CLI has no delimiter
      // escaping; validation rejects only requested unsaved path overrides.
      if (field.kind == Kind::kPath &&
          (text.contains(',') || text.contains('=') || text.contains(QChar::Null) || text.contains('\n')))
        continue;
      append(field.key, field.kind == Kind::kPath && !text.isEmpty() ? Absolute(text, structural_directory_) : text);
    }
  }
  return result;
}
QString PlayerAnalyticsControls::selectedModel(const QString& key) const {
  for (const auto& field : fields_)
    if (field.key == key)
      return value(field).toString();
  return {};
}
QString PlayerAnalyticsControls::validation(bool inspect_files) const {
  try {
    for (const auto& field : fields_)
      if (field.kind == Kind::kFlag && !field.valid && !changed(field))
        return "Invalid saved boolean for " + field.key;
    const YAML::Node effective = effectiveConfig();
    const auto edited_path = [this](const QString& key) {
      for (const auto& field : fields_)
        if (field.key == key)
          return changed(field);
      return false;
    };
    const auto parsed = pa::ParseConfig(Get(effective, "pipeline.player-analytics"));
    if (!parsed.ok())
      return QString::fromUtf8(parsed.status().message().data(), parsed.status().message().size());
    const bool reid = Flag(Get(effective, "pipeline.tracker.reid-enable"));
    const auto gpu = Get(effective, "pipeline.player-analytics.gpu-id");
    if (parsed->enabled() && gpu.IsDefined() && gpu.as<int>() < 0)
      return "Player analytics GPU ID must be nonnegative.";
    if ((parsed->enabled() || reid) && !Flag(Get(effective, "pipeline.tracker.enable")))
      return "Player analytics and the ReID extension require pipeline.tracker.enable=1.";
    if (parsed->enabled() && !Flag(Get(effective, "pipeline.primary-gie.enable")))
      return "Player analytics require pipeline.primary-gie.enable=1.";
    if (parsed->jersey.enabled && parsed->jersey_roi_mode == pa::JerseyRoiMode::kPose && parsed->maximum_due_rois < 2)
      return "Pose-guided jersey recognition requires max-due-rois >= 2.";
    const std::array<std::pair<const char*, const pa::FeatureConfig*>, 3> features{
        {{"pose", &parsed->pose}, {"jersey", &parsed->jersey}, {"action", &parsed->action}}};
    for (const auto& [name, feature] : features) {
      if (!feature->enabled || feature->model != "custom")
        continue;
      const QString path = Absolute(QString::fromStdString(feature->bundle), structural_directory_);
      if (edited_path(kAnalytics + QString(name) + ".bundle") &&
          (path.contains(',') || path.contains('=') || path.contains(QChar::Null) || path.contains('\n')))
        return "This unsaved custom model path cannot be passed in runner arguments (comma, equals sign, NUL or newline). Save Preset first.";
      if (!inspect_files)
        continue;
      if (!QFileInfo(path).isDir())
        return QString("Missing %1 custom model folder: %2").arg(name, path);
      const auto manifest = pa::ParseModelManifest(Document(QDir(path).filePath("manifest.json")));
      if (!manifest.ok())
        return QString("Invalid %1 prepared manifest: %2")
            .arg(name, QString::fromUtf8(manifest.status().message().data(), manifest.status().message().size()));
      const auto expected = std::string(name) == "pose" ? pa::ModelFeature::kPose
          : std::string(name) == "jersey"               ? pa::ModelFeature::kJersey
                                                        : pa::ModelFeature::kAction;
      if (manifest->feature != expected || manifest->maximum_batch < parsed->batch_size)
        return QString("The %1 custom model has the wrong feature or insufficient batch capacity.").arg(name);
      for (const auto& file : {manifest->engine_file, manifest->onnx_file})
        if (!ReadableFile(QDir(path).filePath(QString::fromStdString(file))))
          return QString("Missing or unreadable prepared %1 file: %2").arg(name, QString::fromStdString(file));
    }
    if (reid) {
      const auto model = selectedModel("pipeline.tracker.reid-model");
      if (model != "custom") {
        if (!pa::FindModel("reid", model.toStdString()))
          return "Unsupported player appearance model: " + model;
        return {};
      }
      const auto configured = Get(effective, "pipeline.tracker.reid-config-file");
      if (!configured.IsDefined() || !configured.IsScalar() || configured.as<std::string>().empty())
        return "The custom appearance model requires a prepared configuration file.";
      const QString path = Absolute(QString::fromStdString(configured.as<std::string>()), structural_directory_);
      if (edited_path("pipeline.tracker.reid-config-file") &&
          (path.contains(',') || path.contains('=') || path.contains(QChar::Null) || path.contains('\n')))
        return "This unsaved ReID path cannot be passed in runner arguments (comma, equals sign, NUL or newline). Save Preset first.";
      if (inspect_files) {
        const auto document = Document(path);
        const auto engine = Get(document, "ReID.modelEngineFile");
        if (!engine.IsDefined() || !engine.IsScalar() || engine.as<std::string>().empty() ||
            !ReadableFile(Absolute(QString::fromStdString(engine.as<std::string>()), QFileInfo(path).absolutePath())))
          return "The ReID configuration requires a readable, nonempty prepared modelEngineFile.";
      }
    }
    return {};
  } catch (const std::exception& error) {
    return QString::fromUtf8(error.what());
  }
}
void PlayerAnalyticsControls::updateStatus() {
  for (const auto& field : fields_) {
    if (field.kind != Kind::kPath)
      continue;
    const bool custom = selectedModel(ModelKey(field.key)) == "custom";
    field.row->setVisible(custom);
    field.label->setVisible(custom);
  }
  const auto error = validation(false);
  status_->setText(
      error.isEmpty() ? "Changes apply on the next run. Save Preset keeps these choices. Standard models are "
                        "downloaded and prepared automatically when needed, then reused for this GPU."
                      : error);
}
QString PlayerAnalyticsControls::validateForRun() {
  const auto error = validation(true);
  if (error.isEmpty())
    updateStatus();
  else
    status_->setText(error);
  return error;
}
