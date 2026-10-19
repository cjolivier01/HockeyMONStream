#include "src/apps/hstream-ui/RinkFramingControls.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

namespace {
constexpr const char* keys[]{
    "rink-camera-enable",
    "rink-camera-anticipate",
    "rink-camera-frame-goals",
    "rink-camera-adaptive-zoom"};
constexpr bool defaults[]{false, true, false, false};
YAML::Node options(const YAML::Node& root) {
  YAML::Node result = root;
  for (const char* key : {"pipeline", "ds-playtracker", "private-properties"}) {
    if (!result.IsMap())
      return YAML::Node(YAML::NodeType::Undefined);
    const auto child = static_cast<const YAML::Node&>(result)[key];
    if (!child.IsDefined())
      return YAML::Node(YAML::NodeType::Undefined);
    result.reset(child);
  }
  return result;
}
} // namespace

RinkFramingControls::RinkFramingControls(QWidget* parent) : QWidget(parent) {
  setObjectName("rinkFramingControls");
  auto* layout = new QVBoxLayout(this);
  auto* help = new QLabel(
      "Rink framing (next run). Applies to Program playback and Program archives, including saved job scripts.");
  help->setWordWrap(true);
  layout->addWidget(help);
  const char* labels[]{
      "Enable rink-aware framing",
      "Anticipate the direction of play (keeps native zoom)",
      "Include the nearby goal (can widen the view)",
      "Adaptive zoom (can widen for rushes)"};
  const char* names[]{"rinkFramingEnable", "rinkFramingAnticipate", "rinkFramingGoals", "rinkFramingZoom"};
  for (size_t i = 0; i < choices_.size(); ++i) {
    choices_[i] = new QCheckBox(labels[i]);
    choices_[i]->setObjectName(names[i]);
    choices_[i]->setChecked(defaults[i]);
    layout->addWidget(choices_[i]);
    connect(choices_[i], &QCheckBox::toggled, this, [this] {
      if (!loading_) {
        refresh();
        if (changed_)
          changed_();
      }
    });
  }
  status_ = new QLabel();
  status_->setObjectName("rinkFramingStatus");
  status_->setWordWrap(true);
  layout->addWidget(status_);
  auto* requirement = new QLabel(
      "Requires a saved, reviewed rink map matching the current calibration. "
      "Open Stitched → Rink → Rink map and goals to see what is missing. "
      "Incomplete or stale maps use the native camera and report the reason. "
      "Showing the mini-map is optional. Ice-mask filtering stays the same.");
  requirement->setWordWrap(true);
  layout->addWidget(requirement);
  layout->addStretch();
  refresh();
}

void RinkFramingControls::loadConfig(const YAML::Node& baseline, const YAML::Node& user, const YAML::Node& game) {
  loading_ = true;
  for (size_t i = 0; i < choices_.size(); ++i) {
    bool value = defaults[i];
    for (const auto& layer : {baseline, user, game}) {
      const auto config = options(layer);
      if (config.IsMap() && config[keys[i]].IsDefined()) {
        const auto node = config[keys[i]];
        const auto text = node.as<std::string>();
        if (text == "0" || text == "1")
          value = text == "1";
        else
          value = node.as<bool>();
      }
    }
    choices_[i]->setChecked(value);
    loaded_[i] = value;
  }
  loading_ = false;
  refresh();
}

void RinkFramingControls::resetToDefaults() {
  for (size_t i = 0; i < choices_.size(); ++i)
    choices_[i]->setChecked(defaults[i]);
}
bool RinkFramingControls::isDirty() const {
  for (size_t i = 0; i < choices_.size(); ++i)
    if (choices_[i]->isChecked() != loaded_[i])
      return true;
  return false;
}
void RinkFramingControls::applyChanges(YAML::Node& destination) const {
  for (size_t i = 0; i < choices_.size(); ++i)
    if (choices_[i]->isChecked() != loaded_[i])
      destination["pipeline"]["ds-playtracker"]["private-properties"][keys[i]] = int(choices_[i]->isChecked());
}
QStringList RinkFramingControls::arguments() const {
  QStringList result;
  for (size_t i = 0; i < choices_.size(); ++i)
    result << QString("--options=pipeline.ds-playtracker.private-properties.%1=%2")
                  .arg(keys[i])
                  .arg(int(choices_[i]->isChecked()));
  return result;
}
void RinkFramingControls::refresh() {
  for (size_t i = 1; i < choices_.size(); ++i)
    choices_[i]->setEnabled(choices_[0]->isChecked());
  status_->setText(
      !choices_[0]->isChecked() ? "Off — Program uses the native camera."
          : !choices_[1]->isChecked() && !choices_[2]->isChecked() && !choices_[3]->isChecked()
          ? "No adjustments selected — Program uses the native camera."
          : "Selected for the next Program run or archive job. Map readiness is checked at startup.");
}
void RinkFramingControls::showRunStatus(const QString& message) {
  status_->setText("Current run: " + message);
}
