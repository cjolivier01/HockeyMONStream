#include "Int8PreparationDialog.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace hm::ui {
Int8PreparationDialog::Int8PreparationDialog(Int8PreparationRequest request, QWidget* parent)
    : QDialog(parent), request_(std::move(request)) {
  setObjectName("int8PreparationDialog");
  setWindowTitle("Prepare INT8 from recording");
  resize(680, 470);
  auto* layout = new QVBoxLayout(this);
  auto* description = new QLabel(
      "Samples stitched views across the whole recording, then calibrates and builds an INT8 detector. "
      "Complete stitching calibration first. Preparation can take several minutes. "
      "Check detection accuracy before using INT8 for production output.");
  description->setWordWrap(true);
  layout->addWidget(description);
  layout->addWidget(new QLabel("Calibration frames"));
  count_ = new QSpinBox;
  count_->setObjectName("int8SampleCount");
  count_->setRange(16, 256);
  count_->setValue(64);
  layout->addWidget(count_);
  play_ = new QCheckBox("Play from the beginning when ready");
  play_->setObjectName("int8PlayWhenReady");
  play_->setChecked(true);
  layout->addWidget(play_);
  status_ = new QLabel("The current detector stays selected until preparation succeeds.");
  status_->setObjectName("int8PreparationStatus");
  status_->setWordWrap(true);
  layout->addWidget(status_);
  log_ = new QPlainTextEdit;
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(500);
  layout->addWidget(log_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  prepare_ = buttons->addButton("Prepare INT8", QDialogButtonBox::ActionRole);
  prepare_->setObjectName("int8PrepareButton");
  layout->addWidget(buttons);
  connect(prepare_, &QPushButton::clicked, this, [this] { start(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &Int8PreparationDialog::reject);
  process_.setProcessChannelMode(QProcess::MergedChannels);
  connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      status_->setText("Could not start preparation: " + process_.errorString());
      prepare_->setEnabled(true);
      count_->setEnabled(true);
    }
  });
  connect(
      &process_,
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      this,
      [this](int code, QProcess::ExitStatus status) {
        readOutput();
        if (cancelled_) {
          QDialog::reject();
          return;
        }
        const QFileInfo engine(engine_);
        if (code == 0 && status == QProcess::NormalExit && engine.isFile() && engine.size() > 0 &&
            QFileInfo(manifest_).isFile()) {
          accept();
        } else {
          engine_.clear();
          manifest_.clear();
          status_->setText("Preparation failed. The previous detector remains selected; details are below.");
          prepare_->setEnabled(true);
          count_->setEnabled(true);
        }
      });
}

Int8PreparationDialog::~Int8PreparationDialog() {
  if (process_.state() != QProcess::NotRunning) {
    process_.terminate();
    if (!process_.waitForFinished(5000)) {
      process_.kill();
      process_.waitForFinished(1000);
    }
  }
}

void Int8PreparationDialog::start() {
  const auto& env = request_.environment;
  const QString script = env.value(
      "HSTREAM_INT8_PREPARER", QDir(request_.working_directory).filePath("scripts/prepare_recording_int8.py"));
  const QString builder = env.value(
      "HSTREAM_INT8_BUILDER", QDir(request_.bazel_bin).filePath("src/apps/int8-calib-builder/int8-calib-builder"));
  const QString assets = QDir(request_.bazel_bin).filePath("src/apps/hstream-assets/hstream-assets");
  if (!QFileInfo(script).isFile() || !QFileInfo(builder).isExecutable() || !QFileInfo(assets).isExecutable()) {
    status_->setText(
        "INT8 preparation tools are unavailable. Build the source-checkout preparation tools as described in docs/detection-precision.md.");
    return;
  }
  cancelled_ = false;
  engine_.clear();
  manifest_.clear();
  output_.clear();
  log_->clear();
  prepare_->setEnabled(false);
  count_->setEnabled(false);
  status_->setText("Checking preparation tools and sampling the recording…");
  process_.setWorkingDirectory(request_.working_directory);
  process_.setProcessEnvironment(env);
  process_.setProgram(env.value("HSTREAM_INT8_PYTHON", "python3"));
  process_.setArguments(
      {script,
       "--cli",
       request_.runner,
       "--builder",
       builder,
       "--assets",
       assets,
       "--app-config",
       request_.app_config,
       "--detector-config",
       request_.detector_config,
       "--game-id",
       request_.game_id,
       "--samples",
       QString::number(count_->value())});
  process_.start();
}

void Int8PreparationDialog::readOutput() {
  output_ += process_.readAllStandardOutput();
  while (output_.contains('\n')) {
    const auto newline = output_.indexOf('\n');
    const QByteArray line = output_.left(newline);
    output_.remove(0, newline + 1);
    log_->appendPlainText(QString::fromUtf8(line));
    if (line.startsWith("HSTREAM_INT8_READY ")) {
      const auto result = QJsonDocument::fromJson(line.mid(sizeof("HSTREAM_INT8_READY ") - 1)).object();
      engine_ = result.value("engine").toString();
      manifest_ = result.value("manifest").toString();
    } else if (line.startsWith("HSTREAM_INT8_SAMPLE ")) {
      status_->setText("Sampling stitched frames: " + QString::fromUtf8(line.mid(sizeof("HSTREAM_INT8_SAMPLE ") - 1)));
    } else if (line.contains("stage=quantizing"))
      status_->setText("Calibrating the INT8 model…");
    else if (line.contains("stage=building"))
      status_->setText("Building and checking the INT8 engine…");
  }
  if (output_.size() > 65536) {
    log_->appendPlainText(QString::fromUtf8(output_.left(65536)));
    output_.clear();
  }
}

bool Int8PreparationDialog::playWhenReady() const {
  return play_->isChecked();
}

void Int8PreparationDialog::reject() {
  if (cancelled_)
    return;
  if (process_.state() == QProcess::NotRunning) {
    QDialog::reject();
    return;
  }
  cancelled_ = true;
  status_->setText("Cancelling preparation…");
  process_.terminate();
  QTimer::singleShot(5000, this, [this] {
    if (process_.state() != QProcess::NotRunning)
      process_.kill();
  });
}
} // namespace hm::ui
