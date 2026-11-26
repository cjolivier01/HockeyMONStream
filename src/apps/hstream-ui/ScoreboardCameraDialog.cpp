#include "src/apps/hstream-ui/ScoreboardCameraDialog.h"
#include "hstream/src/libs/media/Mp4VideoTrack.h"
#include "hstream/src/libs/media/VideoStill.h"
#include "hstream/src/libs/scoreboard/CameraConfig.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/PreviewDialogWindow.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

#include <QtCore/QDir>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <cmath>

ScoreboardCameraDialog::ScoreboardCameraDialog(const YAML::Node& camera, const QString& directory, QWidget* parent)
    : QDialog(parent, Qt::Window), camera_(YAML::Clone(camera)), game_directory_(directory) {
  setObjectName("scoreboardCameraDialog");
  setWindowTitle("Extra scoreboard camera");
  configure_preview_dialog_window(this);
  resize(850, 760);
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout;
  enabled_ = new QCheckBox("Use a separate scoreboard camera");
  enabled_->setObjectName("scoreboardCameraEnabled");
  enabled_->setChecked(camera_["enabled"].as<bool>());
  form->addRow(enabled_);
  mode_ = new QComboBox;
  mode_->setObjectName("scoreboardCameraMode");
  mode_->addItem("Recorded video", "recorded");
  mode_->addItem("Live RTSP feed", "live");
  mode_->setCurrentIndex(camera_["uri"].IsNull() ? 0 : 1);
  form->addRow("Source:", mode_);
  files_ = new QPlainTextEdit;
  files_->setObjectName("scoreboardCameraFiles");
  files_->setPlaceholderText("One recording chapter per line, in playback order");
  files_->setMaximumHeight(70);
  QStringList files;
  for (const auto& file : camera_["files"])
    files << QString::fromStdString(file.as<std::string>());
  files_->setPlainText(files.join('\n'));
  auto* file_row = new QHBoxLayout;
  browse_ = new QPushButton(action_icon(ActionIcon::Open), "Choose files…");
  file_row->addWidget(files_);
  file_row->addWidget(browse_);
  form->addRow("Recordings:", file_row);
  uri_ = new QLineEdit(camera_["uri"].IsNull() ? QString() : QString::fromStdString(camera_["uri"].as<std::string>()));
  uri_->setObjectName("scoreboardCameraUri");
  form->addRow("Live address:", uri_);
  codec_ = new QComboBox;
  codec_->setObjectName("scoreboardCameraCodec");
  codec_->addItem("H.264", "h264");
  codec_->addItem("HEVC", "hevc");
  if (!camera_["codec"].IsNull())
    codec_->setCurrentIndex(codec_->findData(QString::fromStdString(camera_["codec"].as<std::string>())));
  form->addRow("Live codec:", codec_);
  ntp_ = new QCheckBox("Cameras share synchronized sender NTP timestamps");
  ntp_->setObjectName("scoreboardCameraNtp");
  ntp_->setChecked(camera_["live_ntp_sync"].as<bool>());
  form->addRow(ntp_);
  offset_ = new QLineEdit(QString::fromStdString(camera_["start_offset"].as<std::string>()));
  offset_->setObjectName("scoreboardCameraOffset");
  offset_->setToolTip(
      "HH:MM:SS.ssss. Positive skips into the scoreboard recording. Negative leaves the scoreboard hidden until that delay has elapsed.");
  form->addRow("Start offset:", offset_);
  preview_time_ = new QLineEdit(QString::fromStdString(camera_["preview_time"].as<std::string>()));
  preview_time_->setObjectName("scoreboardCameraPreviewTime");
  form->addRow("Preview time:", preview_time_);
  auto* controls = new QWidget;
  controls->setLayout(form);
  auto* scroll = new QScrollArea;
  scroll->setObjectName("scoreboardCameraControls");
  scroll->setWidgetResizable(true);
  scroll->setMinimumHeight(120);
  scroll->setMaximumHeight(340);
  scroll->setWidget(controls);
  layout->addWidget(scroll);
  auto* tools = new QHBoxLayout;
  preview_ = new QPushButton(action_icon(ActionIcon::Camera), "Load frame");
  preview_->setObjectName("scoreboardCameraPreview");
  auto* clear = new QPushButton(action_icon(ActionIcon::Reset), "Clear corners");
  auto* fit = new QPushButton(action_icon(ActionIcon::Fit), "Fit");
  tools->addWidget(preview_);
  tools->addWidget(clear);
  tools->addWidget(fit);
  tools->addStretch();
  tools->addWidget(new PreviewDialogWindowSizeButton(this));
  layout->addLayout(tools);
  auto* help = new QLabel(
      "Select corners in order: top left, top right, bottom right, bottom left. Drag to adjust. Scroll over the image to zoom.");
  help->setWordWrap(true);
  layout->addWidget(help);
  canvas_ = new ScoreboardSelectionCanvas(this);
  canvas_->selectionChanged = [this] {
    if (loaded_)
      crop_changed_ = true;
  };
  layout->addWidget(canvas_, 1);
  status_ = new QLabel;
  status_->setWordWrap(true);
  status_->setObjectName("scoreboardCameraStatus");
  layout->addWidget(status_);
  auto* buttons = new QHBoxLayout;
  auto* cancel = new QPushButton(action_icon(ActionIcon::Cancel), "Cancel");
  save_ = new QPushButton(action_icon(ActionIcon::Save), "Save");
  save_->setObjectName("scoreboardCameraSave");
  buttons->addStretch();
  buttons->addWidget(cancel);
  buttons->addWidget(save_);
  layout->addLayout(buttons);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(save_, &QPushButton::clicked, this, &ScoreboardCameraDialog::save);
  connect(preview_, &QPushButton::clicked, this, &ScoreboardCameraDialog::preview);
  connect(clear, &QPushButton::clicked, canvas_, &ScoreboardSelectionCanvas::clearPoints);
  connect(fit, &QPushButton::clicked, canvas_, &ScoreboardSelectionCanvas::fitImage);
  connect(browse_, &QPushButton::clicked, this, [this] {
    auto files = QFileDialog::getOpenFileNames(
        this, "Scoreboard recording chapters", game_directory_, "MP4 video (*.mp4 *.MP4)");
    if (!files.isEmpty())
      files_->setPlainText(files.join('\n'));
  });
  connect(enabled_, &QCheckBox::toggled, this, &ScoreboardCameraDialog::refresh);
  connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
    sourceChanged();
    refresh();
  });
  connect(files_, &QPlainTextEdit::textChanged, this, &ScoreboardCameraDialog::sourceChanged);
  connect(uri_, &QLineEdit::textChanged, this, &ScoreboardCameraDialog::sourceChanged);
  connect(codec_, &QComboBox::currentIndexChanged, this, &ScoreboardCameraDialog::sourceChanged);
  refresh();
}
ScoreboardCameraDialog::~ScoreboardCameraDialog() {
  cancelled_.store(true);
  if (worker_.joinable())
    worker_.join();
}
void ScoreboardCameraDialog::refresh() {
  const bool editable = enabled_->isChecked() && !busy_;
  enabled_->setEnabled(!busy_);
  const bool live = mode_->currentData() == "live";
  mode_->setEnabled(editable);
  files_->setEnabled(editable && !live);
  browse_->setEnabled(editable && !live);
  uri_->setEnabled(editable && live);
  codec_->setEnabled(editable && live);
  ntp_->setEnabled(editable && live);
  offset_->setEnabled(editable);
  preview_time_->setEnabled(editable && !live);
  preview_->setEnabled(editable);
  save_->setEnabled(!busy_);
}
void ScoreboardCameraDialog::sourceChanged() {
  camera_["polygon"] = YAML::Node(YAML::NodeType::Null);
  camera_["source_dimensions"] = YAML::Node(YAML::NodeType::Null);
  loaded_ = false;
  canvas_->clearPoints();
  status_->setText("Source changed. Load a frame and select its scoreboard corners.");
}
YAML::Node ScoreboardCameraDialog::selection() const {
  auto result = YAML::Clone(camera_);
  result["enabled"] = enabled_->isChecked();
  const bool live = mode_->currentData() == "live";
  result["files"] = YAML::Node(YAML::NodeType::Sequence);
  if (!live)
    for (const auto& file : files_->toPlainText().split('\n', Qt::SkipEmptyParts))
      result["files"].push_back(file.trimmed().toStdString());
  result["uri"] = live ? YAML::Node(uri_->text().trimmed().toStdString()) : YAML::Node(YAML::NodeType::Null);
  result["codec"] =
      live ? YAML::Node(codec_->currentData().toString().toStdString()) : YAML::Node(YAML::NodeType::Null);
  result["start_offset"] = offset_->text().trimmed().toStdString();
  result["live_ntp_sync"] = ntp_->isChecked();
  result["preview_time"] = preview_time_->text().trimmed().toStdString();
  if (loaded_ && crop_changed_) {
    result["polygon"] = YAML::Node(YAML::NodeType::Sequence);
    for (const auto& point : canvas_->points()) {
      YAML::Node xy;
      xy.push_back(point.x());
      xy.push_back(point.y());
      result["polygon"].push_back(xy);
    }
  }
  return result;
}
void ScoreboardCameraDialog::save() {
  try {
    const auto result = selection();
    auto valid = hm::scoreboard::ResolveCameraConfig(
        result, game_directory_.toStdString(), hm::scoreboard::CameraDecodeBackend::kNvidia, 0, {});
    if (!valid.ok()) {
      status_->setText(QString::fromStdString(valid.status().ToString()));
      return;
    }
    camera_ = result;
    accept();
  } catch (const std::exception& error) {
    status_->setText(error.what());
  }
}
void ScoreboardCameraDialog::preview() {
  if (busy_)
    return;
  auto settings = selection();
  const bool live = mode_->currentData() == "live";
  const auto parsed = hm::scoreboard::ParseCameraStartOffset(preview_time_->text().trimmed().toStdString());
  if (!live && (!parsed.ok() || *parsed < 0)) {
    status_->setText("Preview time must be a nonnegative HH:MM:SS.ssss value.");
    return;
  }
  if (!live && settings["files"].size() == 0) {
    status_->setText("Choose a scoreboard recording first.");
    return;
  }
  if (worker_.joinable())
    worker_.join();
  cancelled_.store(false);
  busy_ = true;
  refresh();
  status_->setText("Loading a camera frame…");
  const auto directory = game_directory_.toStdString();
  worker_ = std::thread([this, settings, directory, live, seek = live ? int64_t(0) : *parsed] {
    absl::StatusOr<hm::media::VideoStill> result = absl::InternalError("Camera preview did not finish");
    try {
      const auto timeout_ms = settings["operation_timeout_ms"].as<int64_t>();
      if (timeout_ms <= 0 || timeout_ms > INT64_MAX / 1000000)
        throw std::invalid_argument("Invalid scoreboard preview timeout");
      const auto timeout = timeout_ms * 1000000;
      auto cancelled = [this] { return cancelled_.load(); };
      std::string source;
      int64_t local = seek;
      absl::Status status;
      if (live)
        source = settings["uri"].as<std::string>();
      else {
        for (const auto& file : settings["files"]) {
          auto path = std::filesystem::path(file.as<std::string>());
          if (path.is_relative())
            path = std::filesystem::path(directory) / path;
          auto track = hm::media::ReadMp4VideoTrack(path, timeout, cancelled);
          if (!track.ok()) {
            status = track.status();
            break;
          }
          if (local < track->duration_ns) {
            source = path.string();
            break;
          }
          local -= track->duration_ns;
        }
        if (status.ok() && source.empty())
          status = absl::OutOfRangeError("Preview time is beyond the scoreboard recording");
      }
      result = status.ok() ? hm::media::ReadVideoStill(
                                 source,
                                 live,
                                 local,
                                 settings["preview_max_dimension"].as<int>(),
                                 settings["preview_max_source_pixels"].as<int64_t>(),
                                 timeout,
                                 cancelled)
                           : absl::StatusOr<hm::media::VideoStill>(status);
    } catch (const std::exception& error) {
      result = absl::InvalidArgumentError(error.what());
    }
    if (cancelled_.load())
      return;
    QMetaObject::invokeMethod(
        this,
        [this, result = std::move(result)] {
          busy_ = false;
          refresh();
          if (!result.ok()) {
            status_->setText(QString::fromStdString(result.status().ToString()));
            return;
          }
          QImage image(result->rgba.data(), result->width, result->height, result->width * 4, QImage::Format_RGBA8888);
          loaded_ = false;
          canvas_->setPreview(image.copy(), {result->source_width, result->source_height});
          camera_["source_dimensions"] = YAML::Node(YAML::NodeType::Sequence);
          camera_["source_dimensions"].push_back(result->source_width);
          camera_["source_dimensions"].push_back(result->source_height);
          QVector<QPoint> points;
          crop_changed_ = false;
          try {
            if (camera_["polygon"].IsSequence()) {
              for (const auto& point : camera_["polygon"]) {
                const auto x = point[0].as<double>(), y = point[1].as<double>();
                if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > result->source_width - 1 ||
                    y > result->source_height - 1)
                  throw std::invalid_argument("Saved crop is outside the source");
                points.push_back({qRound(x), qRound(y)});
              }
            }
          } catch (const std::exception&) {
            points.clear();
            crop_changed_ = true;
          }
          canvas_->setPoints(points);
          loaded_ = true;
          status_->setText("Choose four scoreboard corners, then Save. The game cameras are unchanged.");
        },
        Qt::QueuedConnection);
  });
}
