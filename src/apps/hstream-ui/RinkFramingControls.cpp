#include "src/apps/hstream-ui/RinkFramingControls.h"

#include <sys/stat.h>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <atomic>
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"
#include "src/apps/hstream-ui/ActionIcons.h"

namespace {
constexpr const char* keys[]{
    "rink-camera-enable",
    "rink-camera-anticipate",
    "rink-camera-frame-goals",
    "rink-camera-adaptive-zoom"};
constexpr bool defaults[]{false, true, false, false};
const char* map_files[]{
    "rink-map.yaml",
    "config.yaml",
    "stitching_generation_id",
    "autooptimiser_out.pto",
    "hm_project.pto",
    "stitching_canvas_provenance",
    "left.png",
    "right.png",
    "mapping_0000.tif",
    "mapping_0001.tif",
    "mapping_0000_x.tif",
    "mapping_0000_y.tif",
    "mapping_0001_x.tif",
    "mapping_0001_y.tif",
    "seam_file.png"};
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

struct RinkFramingControls::MapJob {
  std::atomic<bool> cancelled{false};
  absl::Status result;
};

RinkFramingControls::RinkFramingControls(QWidget* parent, MapCheck check) : QWidget(parent), check_(std::move(check)) {
  if (!check_)
    check_ = [](const QString& path, const std::function<bool()>& cancelled) {
      return hm::stitching::LoadPlaybackRinkMap(path.toStdString(), cancelled).status();
    };
  check_timer_.setSingleShot(true);
  check_timer_.setInterval(150);
  connect(&check_timer_, &QTimer::timeout, this, &RinkFramingControls::startMapCheck);
  connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { checkMap(); });
  connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { checkMap(); });
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
  auto* actions = new QHBoxLayout();
  auto* retry = new QPushButton(action_icon(ActionIcon::Inspect), "Check map again");
  retry->setObjectName("rinkFramingCheckMap");
  connect(retry, &QPushButton::clicked, this, [this] { checkMap(true); });
  turn_off_ = new QPushButton(action_icon(ActionIcon::Stop), "Turn off rink-aware framing");
  turn_off_->setObjectName("rinkFramingTurnOff");
  connect(turn_off_, &QPushButton::clicked, this, [this] { choices_[0]->setChecked(false); });
  actions->addWidget(retry);
  actions->addWidget(turn_off_);
  layout->addLayout(actions);
  auto* requirement = new QLabel(
      "Requires a saved, reviewed rink map matching the current calibration. "
      "Open Stitched → Rink → Rink map and goals to see what is missing. "
      "If enabled, missing, incomplete or stale maps block playback and Program jobs. "
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
  choices_[0]->setEnabled(map_ready_);
  choices_[0]->setToolTip(map_ready_ ? "Saved map is ready for this calibration." : map_error_);
  turn_off_->setVisible(choices_[0]->isChecked() && !map_ready_);
  for (size_t i = 1; i < choices_.size(); ++i)
    choices_[i]->setEnabled(map_ready_ && choices_[0]->isChecked());
  status_->setText(
      !map_ready_                     ? (choices_[0]->isChecked() ? "Cannot start: " : "Unavailable: ") + map_error_
          : !choices_[0]->isChecked() ? "Off — Program uses the native camera."
          : !choices_[1]->isChecked() && !choices_[2]->isChecked() && !choices_[3]->isChecked()
          ? "Cannot start: select an adjustment or turn off rink-aware framing."
          : "Selected for the next Program run or archive job. Map readiness is checked at startup.");
}
void RinkFramingControls::showRunStatus(const QString& message) {
  status_->setText("Current run: " + message);
}

RinkFramingControls::~RinkFramingControls() {
  if (job_)
    job_->cancelled = true;
  if (worker_) {
    worker_->wait();
    delete worker_;
  }
}

QByteArray RinkFramingControls::mapSignature() const {
  QByteArray result = directory_.toUtf8();
  for (const auto* name : map_files) {
    struct stat info{};
    const auto path = QDir(directory_).filePath(name).toLocal8Bit();
    result += '\n';
    if (!directory_.isEmpty() && ::stat(path.constData(), &info) == 0) {
      for (auto value :
           {qint64(info.st_dev),
            qint64(info.st_ino),
            qint64(info.st_size),
            qint64(info.st_mtim.tv_sec),
            qint64(info.st_mtim.tv_nsec),
            qint64(info.st_ctim.tv_sec),
            qint64(info.st_ctim.tv_nsec)})
        result += QByteArray::number(value) + ':';
    }
  }
  return result;
}

void RinkFramingControls::setGameDirectory(const QString& directory) {
  directory_ = directory;
  checkMap();
}

void RinkFramingControls::checkMap(bool force) {
  const auto signature = mapSignature();
  if (!force && signature == signature_)
    return;
  signature_ = signature;
  map_ready_ = false;
  map_error_ = directory_.isEmpty() ? "Select a game with a saved, reviewed rink map." : "Checking saved rink map…";
  if (job_)
    job_->cancelled = true;
  check_pending_ = !directory_.isEmpty();
  if (check_pending_)
    check_timer_.start();
  if (!watcher_.files().empty())
    watcher_.removePaths(watcher_.files());
  if (!watcher_.directories().empty())
    watcher_.removePaths(watcher_.directories());
  if (!directory_.isEmpty() && QDir(directory_).exists()) {
    watcher_.addPath(directory_);
    for (const auto* name : map_files) {
      const auto path = QDir(directory_).filePath(name);
      if (QFileInfo::exists(path))
        watcher_.addPath(path);
    }
  }
  refresh();
}

void RinkFramingControls::startMapCheck() {
  if (worker_ || checking_suspended_ || !check_pending_)
    return;
  check_pending_ = false;
  job_ = std::make_shared<MapJob>();
  const auto job = job_;
  const auto directory = directory_;
  const auto check = check_;
  worker_ = QThread::create(
      [job, directory, check] { job->result = check(directory, [job] { return job->cancelled.load(); }); });
  connect(worker_, &QThread::finished, this, [this, job] {
    auto* completed = worker_;
    worker_ = nullptr;
    completed->deleteLater();
    // Watcher delivery may lag a fast Play click or a completed worker.
    checkMap();
    if (!job->cancelled) {
      map_ready_ = job->result.ok();
      map_error_ = map_ready_ ? QString() : QString::fromStdString(std::string(job->result.message()));
      refresh();
    }
    job_.reset();
    if (check_pending_)
      check_timer_.start();
  });
  worker_->start();
}

QString RinkFramingControls::validateForRun() {
  checkMap();
  if (!choices_[0]->isChecked())
    return {};
  if (!map_ready_)
    return "Rink-aware framing cannot start: " + map_error_ +
        " Open Rink map and goals to review and save the map, or turn off rink-aware framing.";
  if (!choices_[1]->isChecked() && !choices_[2]->isChecked() && !choices_[3]->isChecked())
    return "Select a rink-framing adjustment or turn off rink-aware framing.";
  return {};
}

QString RinkFramingControls::validateSavedSettings(const YAML::Node& config) const {
  if (!choices_[0]->isChecked())
    return {};
  YAML::Node calibration = config;
  for (const char* key : {"hstream_ui", "stitching_calibration"}) {
    if (!calibration.IsMap())
      break;
    const auto child = static_cast<const YAML::Node&>(calibration)[key];
    calibration.reset(child.IsDefined() ? child : YAML::Node(YAML::NodeType::Undefined));
  }
  if (calibration.IsMap() && calibration["status"].as<std::string>("") == "complete" &&
      calibration["stale_from"].as<std::string>("").empty())
    return {};
  return "Rink-aware framing cannot be exported: these settings require calibration again. "
         "Complete calibration and review the rink map, or turn off rink-aware framing.";
}

void RinkFramingControls::setCheckingSuspended(bool suspended) {
  if (suspended == checking_suspended_)
    return;
  checking_suspended_ = suspended;
  if (suspended) {
    check_timer_.stop();
    if (job_) {
      job_->cancelled = true;
      check_pending_ = true;
    }
  } else {
    checkMap(true);
  }
}
