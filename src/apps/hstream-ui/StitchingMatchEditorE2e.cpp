// Opt-in real-media/GPU UI test. See docs/stitching-experiments.md.
#include "hstream/src/libs/stitching/CalibrationMatches.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/MatchEditorDialog.h"
#include "src/apps/hstream-ui/RinkLevelingDialog.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"
#include "src/apps/hstream-ui/StitchingExperimentDialog.h"
#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QImageReader>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTimeEdit>
#if QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

namespace {
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T>
T* widget(QObject& parent, const char* name) {
  auto* result = parent.findChild<T*>(name);
  require(result, std::string("Missing widget ") + name);
  return result;
}
void click(QObject& parent, const char* name) {
  auto* button = widget<QPushButton>(parent, name);
  require(button->isEnabled(), std::string("Disabled button ") + name);
  // Modal exec() belongs to a separate event turn so the driver keeps advancing.
  QTimer::singleShot(0, button, &QPushButton::click);
}
void write(const std::filesystem::path& path, const QString& value) {
  std::ofstream(path) << value.toStdString();
}
// Test-driver helpers only. Input is reviewed YAML/JSON, never a native receipt.
QString image_sha256(const std::filesystem::path& path) {
  QFile file(QString::fromStdString(path.string()));
  require(file.open(QIODevice::ReadOnly), "Cannot read retained original image");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  require(hash.addData(&file), "Cannot hash retained original image");
  return QString::fromLatin1(hash.result().toHex());
}

YAML::Node reviewed_frame_identity(const hm::stitching::CalibrationMatchFrame& frame) {
  YAML::Node result;
  for (size_t camera = 0; camera < 2; ++camera) {
    result["source_names"].push_back(std::filesystem::path(frame.source_paths[camera]).filename().string());
    result["source_seconds"].push_back(frame.source_seconds[camera]);
    result["image_sha256"].push_back(image_sha256(frame.images[camera]).toStdString());
    YAML::Node size;
    size.push_back(frame.sizes[camera].width);
    size.push_back(frame.sizes[camera].height);
    result["sizes"].push_back(size);
  }
  return result;
}

void save_review_template(const hm::stitching::CalibrationMatchSet& original, const std::filesystem::path& artifacts) {
  YAML::Node result;
  result["schema"] = 1;
  result["review_notes"] = "";
  result["promotion_review"] = "";
  result["snapshot"] = original.fingerprint;
  for (size_t index = 0; index < original.frames.size(); ++index) {
    auto frame = reviewed_frame_identity(original.frames[index]);
    frame["index"] = index;
    frame["matches"] = YAML::Node(YAML::NodeType::Sequence);
    for (size_t camera = 0; camera < 2; ++camera) {
      const auto name = "pair-" + std::to_string(index) + "-camera-" + std::to_string(camera) + ".png";
      std::filesystem::copy_file(
          original.frames[index].images[camera], artifacts / name, std::filesystem::copy_options::overwrite_existing);
      frame["images"].push_back(name);
    }
    result["frames"].push_back(frame);
  }
  std::ofstream(artifacts / "review-template.yaml") << YAML::Dump(result);
}

float reviewed_typed_coordinate(double value, int size) {
  // QDoubleSpinBox rounds typed/programmatic numeric entry to nine decimal
  // places before MatchEditorDialog converts the value to its float endpoint.
  // Keep the reviewed source bytes unchanged; compare this explicit UI result.
  const double decimal = QString::number(value, 'f', 9).toDouble();
  return std::clamp(float(decimal), 0.0f, std::nextafter(float(size), 0.0f));
}

YAML::Node expected_reviewed_matches(const YAML::Node& review) {
  YAML::Node result(YAML::NodeType::Sequence);
  for (const auto& frame : review["frames"]) {
    YAML::Node expected;
    expected["index"] = frame["index"].as<int>();
    for (const auto& point : frame["matches"]) {
      YAML::Node normalized;
      for (size_t camera = 0; camera < 2; ++camera)
        for (size_t axis = 0; axis < 2; ++axis) {
          const char* role = camera ? "right" : "left";
          const float value =
              reviewed_typed_coordinate(point[role][axis].as<double>(), frame["sizes"][camera][axis].as<int>());
          normalized[role].push_back(static_cast<double>(value));
        }
      expected["matches"].push_back(normalized);
    }
    result.push_back(expected);
  }
  return result;
}

void apply_reviewed_matches(MatchEditorDialog& editor, const YAML::Node& review) {
  const auto original = editor.editedSet();
  require(review["schema"].as<int>() == 1, "Unknown reviewed-match schema");
  require(!review["review_notes"].as<std::string>().empty(), "Missing independent review notes");
  const auto frames = review["frames"];
  require(frames.IsSequence() && frames.size() == original.frames.size(), "Reviewed frame count differs");
  // Validate every captured pair before editing any controls. PNG hashes bind
  // the actual frame, camera order and extraction bytes across Qt and browser.
  for (size_t index = 0; index < frames.size(); ++index) {
    const auto frame = frames[index];
    const auto identity = reviewed_frame_identity(original.frames[index]);
    require(frame["index"].as<size_t>() == index, "Reviewed pair order differs");
    for (size_t camera = 0; camera < 2; ++camera) {
      require(
          frame["image_sha256"][camera].as<std::string>() == identity["image_sha256"][camera].as<std::string>(),
          "Reviewed original camera PNG differs");
      require(
          std::abs(frame["source_seconds"][camera].as<double>() - original.frames[index].source_seconds[camera]) < 1e-6,
          "Reviewed source time differs");
      for (size_t axis = 0; axis < 2; ++axis)
        require(
            frame["sizes"][camera][axis].as<int>() == identity["sizes"][camera][axis].as<int>(),
            "Reviewed camera dimensions differ");
    }
    require(
        frame["matches"].IsSequence() && frame["matches"].size() >= 4 && frame["matches"].size() <= 100,
        "Provide 4–100 independently reviewed matches in every captured pair");
    for (const auto& point : frame["matches"]) {
      require(!point["feature"].as<std::string>().empty(), "Every point needs a reviewed static-feature description");
      for (size_t camera = 0; camera < 2; ++camera)
        for (size_t axis = 0; axis < 2; ++axis) {
          const double coordinate = point[camera ? "right" : "left"][axis].as<double>();
          require(
              std::isfinite(coordinate) && coordinate >= 0 && coordinate < identity["sizes"][camera][axis].as<int>(),
              "Reviewed point lies outside its original camera image");
        }
    }
  }
  for (size_t index = 0; index < frames.size(); ++index) {
    widget<QComboBox>(editor, "matchEditorPair")->setCurrentIndex(static_cast<int>(index));
    auto* selection = widget<QComboBox>(editor, "matchEditorSelection");
    while (selection->count()) {
      const int before = selection->count();
      auto* remove = widget<QPushButton>(editor, "matchEditorDelete");
      require(remove->isEnabled(), "Delete is unavailable before authoring reviewed matches");
      remove->click();
      require(selection->count() == before - 1, "Delete did not remove the selected match");
    }
    for (const auto& point : frames[index]["matches"]) {
      widget<QPushButton>(editor, "matchEditorAdd")->click();
      for (size_t camera = 0; camera < 2; ++camera) {
        auto* view = widget<QGraphicsView>(editor, camera ? "matchEditorRight" : "matchEditorLeft");
        const auto coordinates = point[camera ? "right" : "left"];
        const QPoint pos = view->mapFromScene(QPointF(coordinates[0].as<double>(), coordinates[1].as<double>()));
        require(view->viewport()->rect().contains(pos), "Reviewed point is outside the fitted viewport");
        QMouseEvent press(
            QEvent::MouseButtonPress,
            pos,
            view->viewport()->mapToGlobal(pos),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &press);
        QMouseEvent release(
            QEvent::MouseButtonRelease,
            pos,
            view->viewport()->mapToGlobal(pos),
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &release);
      }
      for (size_t coordinate = 0; coordinate < 4; ++coordinate) {
        const auto name = QString("matchEditorCoordinate%1").arg(coordinate).toLatin1();
        widget<QDoubleSpinBox>(editor, name.constData())
            ->setValue(point[coordinate < 2 ? "left" : "right"][coordinate % 2].as<double>());
      }
    }
    require(
        selection->count() == static_cast<int>(frames[index]["matches"].size()),
        "UI did not retain every reviewed match");
    const auto entered = editor.editedSet().frames[index].matches;
    for (size_t point = 0; point < entered.size(); ++point) {
      const auto wanted = frames[index]["matches"][point];
      const auto endpoint = [&](size_t camera) {
        const char* role = camera ? "right" : "left";
        return cv::Point2f(
            reviewed_typed_coordinate(wanted[role][0].as<double>(), frames[index]["sizes"][camera][0].as<int>()),
            reviewed_typed_coordinate(wanted[role][1].as<double>(), frames[index]["sizes"][camera][1].as<int>()));
      };
      require(
          entered[point].left == endpoint(0) && entered[point].right == endpoint(1),
          "Ordinary coordinate controls changed a reviewed endpoint");
    }
  }
}

StitchingExperimentCatalog read_catalog(const std::filesystem::path& game) {
  const auto store = OpenStitchingExperimentStore(game);
  require(store.ok(), store.status().ToString());
  const auto catalog = LoadStitchingExperimentStore(*store);
  require(catalog.ok(), catalog.status().ToString());
  return *catalog;
}

std::set<std::string> workspace_keys(const std::filesystem::path& game) {
  std::set<std::string> result;
  for (const auto& record : read_catalog(game).experiments)
    result.insert(record.workspace.game_directory.string());
  return result;
}

StoredStitchingExperiment added_record(
    const std::filesystem::path& game,
    const std::set<std::string>& before,
    QTableWidget& table,
    int row) {
  const auto sequence = QRegularExpression("(\\d+)$").match(table.item(row, 0)->text());
  require(sequence.hasMatch(), "Candidate row has no sequence identity");
  std::optional<StoredStitchingExperiment> found;
  for (const auto& record : read_catalog(game).experiments)
    if (!before.count(record.workspace.game_directory.string()) && record.sequence == sequence.captured(1).toInt()) {
      require(!found, "New candidate sequence is ambiguous");
      found = record;
    }
  require(found.has_value(), "New candidate has no durable native catalog record");
  return *found;
}

StoredStitchingExperiment current_record(const std::filesystem::path& game, const std::filesystem::path& workspace) {
  for (const auto& record : read_catalog(game).experiments)
    if (record.workspace.game_directory == workspace)
      return record;
  throw std::runtime_error("Owned candidate disappeared from its native catalog");
}

void same_matches(
    const hm::stitching::CalibrationMatchSet& actual,
    const hm::stitching::CalibrationMatchSet& expected) {
  require(actual.frames.size() == expected.frames.size(), "Saved editor lost original pairs");
  for (size_t index = 0; index < expected.frames.size(); ++index) {
    const auto& a = actual.frames[index];
    const auto& e = expected.frames[index];
    require(a.sizes == e.sizes && a.source_seconds == e.source_seconds, "Saved original frame identity changed");
    for (size_t camera = 0; camera < 2; ++camera)
      require(image_sha256(a.images[camera]) == image_sha256(e.images[camera]), "Saved original PNG bytes changed");
    require(a.matches.size() == e.matches.size(), "Saved manual points were refilled or removed");
    for (size_t point = 0; point < e.matches.size(); ++point)
      require(
          a.matches[point].left == e.matches[point].left && a.matches[point].right == e.matches[point].right,
          "Saved manual coordinates changed");
  }
}

bool preview_process_matches(const StoredStitchingExperiment& record) {
  if (record.process_session_id <= 0 || record.process_token.empty())
    return false;
  QFile file(QString("/proc/%1/environ").arg(record.process_session_id));
  if (!file.open(QIODevice::ReadOnly))
    return false;
  return file.readAll().split('\0').contains(
      QByteArray::fromStdString("HSTREAM_EXPERIMENT_PROCESS_TOKEN=" + record.process_token));
}

QByteArray preview_process_identity(
    const StoredStitchingExperiment& record,
    const std::filesystem::path& runner,
    WId window) {
  require(preview_process_matches(record), "Preview process token changed");
  const QString root = QString("/proc/%1/").arg(record.process_session_id);
  QFile stat(root + "stat"), command(root + "cmdline"), environment(root + "environ");
  require(
      stat.open(QIODevice::ReadOnly) && command.open(QIODevice::ReadOnly) && environment.open(QIODevice::ReadOnly),
      "Preview process is unavailable");
  const QByteArray fields = stat.readAll();
  const auto values = fields.mid(fields.lastIndexOf(')') + 2).simplified().split(' ');
  require(values.size() > 19, "Preview starttime is unavailable");
  require(values[1].toLongLong() == QCoreApplication::applicationPid(), "Preview is not this dialog's child");
  require(
      values[2].toLongLong() == record.process_session_id && values[3].toLongLong() == record.process_session_id &&
          values[19].toULongLong() > 0,
      "Preview process session or starttime is invalid");
  require(
      environment.readAll().split('\0').contains(
          QByteArray::fromStdString("HM_GAME_DIR=" + record.workspace.root.string())),
      "Preview environment does not select the owned candidate directory");
  const QByteArray arguments = command.readAll();
  const auto args = arguments.split('\0');
  const int game_argument = args.indexOf("-g");
  require(
      game_argument >= 0 && game_argument + 1 < args.size() &&
          args[game_argument + 1] == QByteArray::fromStdString(record.workspace.game_id) &&
          args.contains(
              QByteArray("--ui-preview-windows=stitched:") + QByteArray::number(static_cast<qulonglong>(window))) &&
          args.contains("--ui-preview-active=stitched"),
      "Preview command does not target the selected candidate and exact Qt window");
  require(
      QFileInfo(root + "exe").canonicalFilePath() ==
          QFileInfo(QString::fromStdString(runner.string())).canonicalFilePath(),
      "Preview executable changed");
  return values[19] + '\0' + arguments;
}

QSize viewable_preview_size(QWidget* video, YAML::Node& report) {
#if QT_CONFIG(xcb)
  require(QGuiApplication::platformName() == "xcb", "Native front-buffer capture requires X11");
  auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  require(x11 && x11->connection(), "Native preview X11 connection is unavailable");
  auto* connection = x11->connection();
  const auto window = static_cast<xcb_window_t>(video->winId());
  const auto attributes_cookie = xcb_get_window_attributes(connection, window);
  const auto geometry_cookie = xcb_get_geometry(connection, window);
  auto* attributes = xcb_get_window_attributes_reply(connection, attributes_cookie, nullptr);
  auto* geometry = xcb_get_geometry_reply(connection, geometry_cookie, nullptr);
  const bool viewable = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
  const QSize size = geometry ? QSize(geometry->width, geometry->height) : QSize();
  std::free(attributes);
  std::free(geometry);
  report["preview_native_window_viewable"] = viewable;
  report["preview_native_window_width"] = size.width();
  report["preview_native_window_height"] = size.height();
  require(viewable && !size.isEmpty(), "Native preview window is not viewable");
  require(size == video->size() * video->devicePixelRatioF(), "Native and Qt preview dimensions disagree");
  return size;
#else
  throw std::runtime_error("Native front-buffer capture requires X11");
#endif
}

void request_front_buffer(
    const StoredStitchingExperiment& record,
    const std::filesystem::path& runner,
    WId window,
    const QByteArray& identity,
    const QString& path) {
  require(!path.contains('\n') && !path.contains('\r'), "Capture path contains a runtime-command separator");
  require(!QFileInfo::exists(path), "Native capture output already exists");
  require(preview_process_identity(record, runner, window) == identity, "Preview identity changed before capture");
  const QByteArray input = QString("/proc/%1/fd/0").arg(record.process_session_id).toLocal8Bit();
  const int descriptor = ::open(input.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  require(descriptor >= 0, "Cannot open owned preview command pipe");
  QFile pipe;
  if (!pipe.open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
    ::close(descriptor);
    throw std::runtime_error("Cannot retain owned preview command pipe");
  }
  struct stat opened{}, current{};
  require(
      ::fstat(descriptor, &opened) == 0 && ::stat(input.constData(), &current) == 0 && S_ISFIFO(opened.st_mode) &&
          opened.st_dev == current.st_dev && opened.st_ino == current.st_ino,
      "Preview stdin is not the same owned pipe");
  require(preview_process_identity(record, runner, window) == identity, "Preview identity changed before command");
  const QByteArray command = "@capture-preview-frame stitched " + path.toUtf8() + '\n';
  require(command.size() <= 4096, "Native capture command exceeds the bounded pipe write");
  require(::write(descriptor, command.constData(), command.size()) == command.size(), "Native capture command failed");
}

bool owned_tokens_retired(const std::set<std::string>& tokens) {
  if (tokens.empty())
    return true;
  for (const auto& name : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    bool numeric = false;
    name.toLongLong(&numeric);
    if (!numeric)
      continue;
    QFile environment("/proc/" + name + "/environ");
    if (!environment.open(QIODevice::ReadOnly))
      continue; // Native catalog release is authoritative; this adds visible-token checks.
    const auto values = environment.readAll().split('\0');
    for (const auto& token : tokens)
      if (values.contains(QByteArray::fromStdString("HSTREAM_EXPERIMENT_PROCESS_TOKEN=" + token)))
        return false;
  }
  return true;
}

struct PixelSample {
  QString sha256;
  double nonblack_fraction{0};
};

PixelSample sample_pixels(const QImage& image) {
  require(!image.isNull(), "Native window capture is unavailable; moving pixels are unqualified");
  const QImage sample =
      image.scaled(96, 54, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
  size_t nonblack = 0;
  for (int y = 0; y < sample.height(); ++y)
    for (int x = 0; x < sample.width(); ++x) {
      const auto color = sample.pixelColor(x, y);
      nonblack += color.red() + color.green() + color.blue() > 36;
    }
  const auto bytes =
      QByteArray(reinterpret_cast<const char*>(sample.constBits()), static_cast<int>(sample.sizeInBytes()));
  return {
      QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()),
      static_cast<double>(nonblack) / static_cast<double>(sample.width() * sample.height())};
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  install_button_icon_style();
  if (argc != 5 && argc != 6) {
    std::cerr << "usage: stitching_match_editor_e2e ISOLATED_GAME_DIRECTORY REPOSITORY ARTIFACT_DIRECTORY RUNNER "
                 "[CALIBRATION_TIME]\n";
    return 2;
  }
  const std::filesystem::path game = std::filesystem::absolute(argv[1]);
  const std::filesystem::path repo = std::filesystem::absolute(argv[2]);
  const std::filesystem::path artifacts = std::filesystem::absolute(argv[3]);
  std::filesystem::create_directories(artifacts);
  if (!std::filesystem::exists(game / ".match-editor-e2e-sandbox")) {
    std::cerr << "Refusing a game without the isolated-test marker\n";
    return 2;
  }
  const QString source_mode = qEnvironmentVariable("HSTREAM_MATCH_EDITOR_SOURCE_MODE", "automatic");
  const std::string reviewed_path = qEnvironmentVariable("HSTREAM_MATCH_EDITOR_REVIEWED_MATCHES").toStdString();
  const std::string retained_preview_report =
      qEnvironmentVariable("HSTREAM_MATCH_EDITOR_RETAINED_PREVIEW_REPORT").toStdString();
  const bool capture_only = qEnvironmentVariableIntValue("HSTREAM_MATCH_EDITOR_CAPTURE_ONLY") == 1;
  const bool reviewed_promote = qEnvironmentVariableIntValue("HSTREAM_MATCH_EDITOR_PROMOTE_REVIEWED") == 1;
  const QString capture_method = qEnvironmentVariable("HSTREAM_MATCH_EDITOR_PREVIEW_CAPTURE", "qscreen");
  if (capture_method != "qscreen" && capture_method != "native-gl-front") {
    std::cerr << "Preview capture must be qscreen or explicit native-gl-front\n";
    return 2;
  }
  const bool native_front_capture = capture_method == "native-gl-front";
  // A runner can exit between ownership validation and the nonblocking pipe
  // write. Report EPIPE through ordinary cleanup instead of killing this driver.
  if (native_front_capture && std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    std::cerr << "Cannot make native capture pipe failures recoverable\n";
    return 2;
  }
  if (!retained_preview_report.empty() && (capture_only || reviewed_promote || reviewed_path.empty())) {
    std::cerr << "Retained preview diagnostics require reviewed input and forbid capture-only or promotion\n";
    return 2;
  }
  if (source_mode != "automatic" && source_mode != "main" && source_mode != "automatic-failed") {
    std::cerr << "Unknown source mode; failed Main is not a supported route in the current Qt product\n";
    return 2;
  }
  if (source_mode == "automatic-failed" && reviewed_path.empty() && !capture_only) {
    std::cerr << "A failed source needs genuinely reviewed matches or capture-only mode\n";
    return 2;
  }
  QByteArray reviewed_bytes;
  YAML::Node reviewed_document;
  if (!reviewed_path.empty()) {
    try {
      QFile file(QString::fromStdString(reviewed_path));
      require(file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024, "Cannot read bounded reviewed input");
      reviewed_bytes = file.readAll();
      reviewed_document = YAML::Load(reviewed_bytes.toStdString());
      require(
          !reviewed_promote || !reviewed_document["promotion_review"].as<std::string>("").empty(),
          "Record independent physical preview review before promoting reviewed matches");
      QFile retained(QString::fromStdString((artifacts / "review-input.yaml").string()));
      require(
          retained.open(QIODevice::WriteOnly) && retained.write(reviewed_bytes) == reviewed_bytes.size(),
          "Cannot retain exact reviewed input evidence");
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 2;
    }
  }
  const int frame_count = qEnvironmentVariableIsSet("HSTREAM_MATCH_EDITOR_FRAME_COUNT")
      ? qEnvironmentVariableIntValue("HSTREAM_MATCH_EDITOR_FRAME_COUNT")
      : 2;
  const int control_points = qEnvironmentVariableIsSet("HSTREAM_MATCH_EDITOR_CONTROL_POINTS")
      ? qEnvironmentVariableIntValue("HSTREAM_MATCH_EDITOR_CONTROL_POINTS")
      : 100;
  if (control_points <= 0) {
    std::cerr << "Choose a positive control-point budget\n";
    return 2;
  }
  if (frame_count < 1 || frame_count > 64 || (reviewed_path.empty() && !capture_only && frame_count != 2)) {
    std::cerr << "Choose a valid pair count; the legacy edit scenario requires two pairs\n";
    return 2;
  }
  bool applied = false;
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert("HM_GAME_DIR", QString::fromStdString(game.parent_path().string()));
  env.insert("HM_OUTPUT_WORK_DIR", QString::fromStdString((artifacts / "output").string()));
  StitchingExperimentDialog dialog(
      QString::fromStdString(game.string()),
      QString::fromLocal8Bit(argv[4]),
      QString::fromStdString(repo.string()),
      QString::fromStdString((repo / "configs/ds_hockey_app_config.yaml").string()),
      env,
      control_points,
      frame_count,
      argc == 6 ? QString::fromLocal8Bit(argv[5]) : QStringLiteral("00:09:42"),
      nullptr,
      [&] { applied = true; });
  dialog.show();
  auto* table = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
  auto* log = widget<QPlainTextEdit>(dialog, "stitchExperimentLog");
  auto* status = widget<QLabel>(dialog, "stitchExperimentStatus");
  widget<QCheckBox>(dialog, "stitchExperimentPreferPlayerFrames")->setChecked(false);
  widget<QCheckBox>(dialog, "stitchExperimentLoop")->setChecked(false);
  widget<QSpinBox>(dialog, "stitchExperimentPreviewDuration")->setValue(20);
  if (qEnvironmentVariableIsSet("HSTREAM_MATCH_EDITOR_PREVIEW_START")) {
    const QTime start = QTime::fromString(qEnvironmentVariable("HSTREAM_MATCH_EDITOR_PREVIEW_START"), "HH:mm:ss");
    require(start.isValid(), "Preview start must be HH:mm:ss");
    widget<QTimeEdit>(dialog, "stitchExperimentPreviewStart")->setTime(start);
  }
  auto* resolution = widget<QComboBox>(dialog, "stitchExperimentControlPointResolution");
  const int two_k = resolution->findData("2k");
  if (two_k >= 0)
    resolution->setCurrentIndex(two_k);
  hm::stitching::CalibrationMatchSet expected;
  QString prior, source_state, source_config_sha, manual_fingerprint;
  std::filesystem::path source_workspace = game, manual_workspace;
  std::set<std::string> before_source, before_manual, owned_tokens;
  for (const auto& record : read_catalog(game).experiments)
    require(
        record.process_session_id == 0 && record.process_token.empty() && record.state != "running" &&
            record.state != "scan" && record.state != "queued" && record.state != "quarantined",
        "The dedicated game must have no queued or owned native work");
  std::optional<StoredStitchingExperiment> preview_owner;
  QString preview_output;
  QString preview_incremental_output;
  qint64 preview_log_offset = -1, preview_log_read = 0;
  bool diagnostic_capture_saved = false;
  QImage last_screen_capture;
  qint64 first_screen_capture_ms = 0;
  QByteArray preview_identity;
  QString native_capture_path;
  int native_capture_requests = 0;
  qint64 native_capture_sent_ms = 0;
  QSize native_capture_size;
  std::optional<qulonglong> first_capture_pts;
  QString preview_generation, first_pixel_hash;
  qint64 first_pixel_ms = 0;
  QSize first_pixel_size;
  WId preview_window = 0;
  YAML::Node report;
  report["source_mode"] = source_mode.toStdString();
  report["capture_only"] = capture_only;
  report["reviewed_points_used"] = !reviewed_path.empty();
  report["retained_preview_only"] = !retained_preview_report.empty();
  if (!reviewed_bytes.isEmpty())
    report["review_input_sha256"] =
        QCryptographicHash::hash(reviewed_bytes, QCryptographicHash::Sha256).toHex().toStdString();
  report["physical_map_qualified"] = false;
  report["preview_capture_method"] = capture_method.toStdString();
  report["renderer_window_presentation_qualified"] = false;
  report["compositor_visibility_qualified"] = false;
  if (native_front_capture)
    report["preview_capture_boundary"] =
        "Explicit bounded GL_FRONT readback proves current pixels on the owned, viewable native window. "
        "It does not qualify desktop compositor visibility or physical rink geometry.";
  report["workflow_qualified"] = false;
  report["cleanup_complete"] = false;
  report["runner_sha256"] = image_sha256(std::filesystem::absolute(argv[4])).toStdString();
  report["pipeline_config_sha256"] = image_sha256(repo / "configs/ds_hockey_app_config.yaml").toStdString();
  report["test_driver_sha256"] = image_sha256(std::filesystem::absolute(argv[0])).toStdString();
  const auto config_root = std::filesystem::path(
      env.value("HM_CONFIG_ROOT", QString::fromStdString((repo / "configs").string())).toStdString());
  for (const char* config : {"baseline.yaml", "ds_hockey_app_config.yaml", "ds_hockey_configure_stitching.yaml"})
    report["config_sha256"][config] = image_sha256(config_root / config).toStdString();
  std::unique_ptr<StitchingExperimentDialog> reopened_dialog;
  bool finishing = false, close_requested = false, reopened_close_requested = false;
  int requested_exit = 1;
  QString pending_result;
  QElapsedTimer cleanup_elapsed;
  int step = 0;
  bool editor_maximized = false;
  int automatic_row = -1;
  int manual_row = -1;
  size_t saved_control_points = 0;
  std::unique_ptr<RinkLevelingDialog> leveling;
  QElapsedTimer elapsed;
  elapsed.start();
  QElapsedTimer preview_elapsed;
  QTimer driver;
  driver.setInterval(100);
  QObject::connect(log->document(), &QTextDocument::contentsChange, &app, [&](int position, int, int added) {
    if (step != 6 || added <= 0)
      return;
    QTextCursor cursor(log->document());
    cursor.setPosition(position);
    cursor.setPosition(std::min(position + added, log->document()->characterCount() - 1), QTextCursor::KeepAnchor);
    preview_incremental_output += cursor.selectedText().replace(QChar::ParagraphSeparator, QChar('\n'));
    preview_incremental_output = preview_incremental_output.right(1024 * 1024);
  });
  const auto finish = [&](int result, const QString& message) {
    write(artifacts / "preview-current-process.log", preview_output);
    write(artifacts / "preview-document-incremental.log", preview_incremental_output);
    if (!last_screen_capture.isNull())
      last_screen_capture.save(QString::fromStdString((artifacts / "preview-screen-last-attempt.png").string()));
    finishing = true;
    requested_exit = result;
    pending_result = message;
    cleanup_elapsed.start();
  };
  QObject::connect(&driver, &QTimer::timeout, &app, [&] {
    try {
      for (const auto& record : read_catalog(game).experiments)
        if (!record.process_token.empty())
          owned_tokens.insert(record.process_token);
      if (finishing) {
        // Keep the event loop alive while the ordinary dialog close path
        // cancels work, retires descendants and publishes ownership release.
        for (auto* window : QApplication::topLevelWidgets()) {
          if (!window->isVisible())
            continue;
          for (const char* name : {"matchEditorDiscardChanges", "stitchExperimentCloseDiscard"})
            if (auto* button = window->findChild<QPushButton*>(name); button && button->isEnabled())
              click(*window, name);
          if (window->objectName() == "stitchExperimentFrameInspector")
            QTimer::singleShot(0, window, [window] { window->close(); });
          if (window->objectName() == "matchEditorDialog" && window->isEnabled())
            click(*window, "matchEditorCancel");
        }
        if (leveling && leveling->isVisible())
          leveling->close();
        if (leveling && !leveling->isVisible())
          leveling.reset(); // Join its owned offline preview work before claiming cleanup.
        if (!close_requested) {
          close_requested = true;
          QTimer::singleShot(0, &dialog, [&dialog] { dialog.close(); });
        }
        if (reopened_dialog && !reopened_close_requested) {
          reopened_close_requested = true;
          QTimer::singleShot(0, reopened_dialog.get(), [window = reopened_dialog.get()] { window->close(); });
        }
        bool retired = !dialog.isVisible() && (!reopened_dialog || !reopened_dialog->isVisible()) &&
            (!leveling || !leveling->isVisible());
        for (auto* window : QApplication::topLevelWidgets())
          if (window->isVisible() &&
              (window->objectName() == "matchEditorDialog" || window->objectName() == "stitchExperimentFrameInspector"))
            retired = false;
        for (const auto& record : read_catalog(game).experiments)
          retired &= record.process_session_id == 0 && record.process_token.empty() && record.state != "running" &&
              record.state != "scan" && record.state != "quarantined";
        if (retired && owned_tokens_retired(owned_tokens)) {
          write(artifacts / "runner.log", log->toPlainText());
          report["cleanup_complete"] = true;
          report["exit_code"] = requested_exit;
          report["workflow_qualified"] =
              requested_exit == 0 && retained_preview_report.empty() && report["promoted"].as<bool>(false);
          write(artifacts / "result.txt", pending_result);
          std::ofstream(artifacts / "workflow-report.yaml") << YAML::Dump(report);
          driver.stop();
          app.exit(requested_exit);
        } else if (cleanup_elapsed.elapsed() > 180000) {
          report["cleanup_error"] = "Ordinary close did not establish terminal ownership within 180s";
          std::ofstream(artifacts / "workflow-report.yaml") << YAML::Dump(report);
          write(artifacts / "failure.txt", pending_result + "\nCleanup remained unverified.\n");
          driver.stop();
          app.exit(1); // Destruction retains the product's final containment fallback.
        }
        return;
      }
      const QString current = status->text();
      if (prior != current) {
        std::cerr << current.toStdString() << '\n';
        prior = current;
      }
      require(elapsed.elapsed() < 1200000, "End-to-end test exceeded 20 minutes");
      write(artifacts / "runner.log", log->toPlainText());
      require(!current.startsWith("Could not persist"), current.toStdString());
      if (step == 0 && !retained_preview_report.empty()) {
        require(std::filesystem::file_size(retained_preview_report) <= 4 * 1024 * 1024, "Prior report is too large");
        const auto prior_report = YAML::LoadFile(retained_preview_report);
        require(prior_report["cleanup_complete"].as<bool>(), "Prior workflow cleanup did not finish");
        require(
            prior_report["review_input_sha256"].as<std::string>() == report["review_input_sha256"].as<std::string>() &&
                prior_report["runner_sha256"].as<std::string>() == report["runner_sha256"].as<std::string>(),
            "Retained preview input or runner differs from the original workflow");
        for (const char* config : {"baseline.yaml", "ds_hockey_app_config.yaml", "ds_hockey_configure_stitching.yaml"})
          require(
              prior_report["config_sha256"][config].as<std::string>() ==
                  report["config_sha256"][config].as<std::string>(),
              "Retained preview configuration differs");
        manual_workspace = prior_report["manual_workspace"].as<std::string>();
        const auto retained = current_record(game, manual_workspace);
        require(
            retained.state == "complete" && retained.process_session_id == 0 && retained.process_token.empty() &&
                retained.artifact_generation_id == prior_report["artifact_generation"].as<std::string>(),
            "Retained manual candidate generation or ownership differs");
        const auto config = YAML::LoadFile((manual_workspace / "config.yaml").string());
        const auto fingerprint = hm::stitching::manual_control_point_fingerprint(config);
        require(
            fingerprint.ok() && *fingerprint == prior_report["manual_fingerprint"].as<std::string>(),
            "Retained manual fingerprint differs");
        const auto saved = hm::stitching::LoadCalibrationMatches(manual_workspace, *fingerprint);
        require(saved.ok() && saved->manual, "Retained manual snapshot is unavailable");
        const auto normalized = expected_reviewed_matches(reviewed_document);
        require(saved->frames.size() == normalized.size(), "Retained manual pair count differs");
        for (size_t index = 0; index < saved->frames.size(); ++index) {
          const auto& frame = saved->frames[index];
          const auto wanted = reviewed_document["frames"][index];
          require(wanted["index"].as<size_t>() == index, "Retained review pair order differs");
          for (size_t camera = 0; camera < 2; ++camera) {
            require(
                image_sha256(frame.images[camera]).toStdString() == wanted["image_sha256"][camera].as<std::string>() &&
                    std::abs(frame.source_seconds[camera] - wanted["source_seconds"][camera].as<double>()) < 1e-6 &&
                    frame.sizes[camera].width == wanted["sizes"][camera][0].as<int>() &&
                    frame.sizes[camera].height == wanted["sizes"][camera][1].as<int>(),
                "Retained original image or source time differs");
          }
          require(frame.matches.size() == normalized[index]["matches"].size(), "Retained manual point count differs");
          for (size_t point = 0; point < frame.matches.size(); ++point) {
            const auto match = normalized[index]["matches"][point];
            require(
                frame.matches[point].left == cv::Point2f(match["left"][0].as<float>(), match["left"][1].as<float>()) &&
                    frame.matches[point].right ==
                        cv::Point2f(match["right"][0].as<float>(), match["right"][1].as<float>()),
                "Retained manual endpoints differ from reviewed numeric entry");
          }
        }
        for (int row = 0; row < table->rowCount(); ++row) {
          const auto sequence = QRegularExpression("(\\d+)$").match(table->item(row, 0)->text());
          if (sequence.hasMatch() && sequence.captured(1).toInt() == retained.sequence) {
            require(manual_row < 0, "Retained candidate row is ambiguous");
            manual_row = row;
          }
        }
        require(
            manual_row >= 0 && table->item(manual_row, 5)->text() == "Ready", "Retained candidate row is not ready");
        source_workspace = prior_report["source_workspace"].as<std::string>();
        source_config_sha = QString::fromStdString(prior_report["source_config_sha256"].as<std::string>());
        require(image_sha256(source_workspace / "config.yaml") == source_config_sha, "Prior source config changed");
        report["retained_from_report"] = retained_preview_report;
        report["retained_report_sha256"] = image_sha256(retained_preview_report).toStdString();
        report["source_workspace"] = source_workspace.string();
        report["source_config_sha256"] = source_config_sha.toStdString();
        report["manual_workspace"] = manual_workspace.string();
        report["manual_fingerprint"] = *fingerprint;
        report["artifact_generation"] = retained.artifact_generation_id;
        report["reviewed_coordinate_normalization"] = "qt-fixed9-float32";
        report["reviewed_expected_matches"] = normalized;
        save_review_template(*saved, artifacts);
        table->selectRow(manual_row);
        preview_log_offset = QFileInfo(QString::fromStdString((manual_workspace / "runner.log").string())).size();
        report["preview_log_start_offset"] = preview_log_offset;
        click(dialog, "previewStitchExperimentButton");
        preview_elapsed.start();
        step = 6;
      } else if (step == 0 && source_mode == "main") {
        for (int row = 0; row < table->rowCount(); ++row)
          if (table->item(row, 0)->text() == "Main calibration")
            automatic_row = row;
        require(automatic_row >= 0, "No retained Main row; failed Main without player selection is currently omitted");
        require(table->item(automatic_row, 5)->text() == "Ready", "This mode requires a genuine completed Main");
        source_state = table->item(automatic_row, 5)->text();
        source_config_sha = image_sha256(game / "config.yaml");
        table->selectRow(automatic_row);
        click(dialog, "inspectStitchExperimentFramesButton");
        step = 3;
      } else if (step == 0) {
        before_source = workspace_keys(game);
        automatic_row = table->rowCount();
        click(dialog, "addStitchExperimentsToBatchButton");
        ++step;
      } else if (
          step == 1 && table->rowCount() > automatic_row &&
          widget<QPushButton>(dialog, "startStitchExperimentBatchButton")->isEnabled()) {
        click(dialog, "startStitchExperimentBatchButton");
        ++step;
      } else if (step == 2 && widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton")->isEnabled()) {
        source_state = table->item(automatic_row, 5)->text();
        require(
            source_mode == "automatic-failed" ? source_state != "Ready" : source_state == "Ready",
            "Unexpected automatic calibration outcome: " + source_state.toStdString());
        if (source_mode == "automatic-failed")
          require(log->toPlainText().contains("status=failed"), "Expected an authentic native calibration failure");
        const auto source = added_record(game, before_source, *table, automatic_row);
        require(
            source.state == (source_mode == "automatic-failed" ? "failed" : "complete"),
            "Native source record does not certify the expected terminal outcome");
        require(source.process_session_id == 0 && source.process_token.empty(), "Source solve still owns its process");
        source_workspace = source.workspace.game_directory;
        source_config_sha = image_sha256(source_workspace / "config.yaml");
        report["source_workspace"] = source_workspace.string();
        report["source_state"] = source.state;
        report["source_config_sha256"] = source_config_sha.toStdString();
        table->selectRow(automatic_row);
        click(dialog, "inspectStitchExperimentFramesButton");
        ++step;
      } else if (step == 3) {
        auto* viewer = dialog.findChild<QDialog*>("stitchExperimentFrameInspector");
        if (viewer && viewer->isVisible()) {
          click(*viewer, "stitchExperimentEditMatches");
          ++step;
        }
      } else if (step == 4) {
        auto* editor = dynamic_cast<MatchEditorDialog*>(dialog.findChild<QDialog*>("matchEditorDialog"));
        if (editor && editor->isVisible()) {
          if (!editor_maximized) {
            editor_maximized = true;
            editor->showMaximized();
            return; // Let native window geometry settle before source-pixel clicks.
          }
          save_review_template(editor->editedSet(), artifacts);
          if (capture_only) {
            editor->reject();
            finish(
                0, "CAPTURE ONLY: actual original pairs retained; no manual solve, preview or promotion qualified.\n");
            return;
          }
          if (!reviewed_path.empty()) {
            apply_reviewed_matches(*editor, reviewed_document);
            report["reviewed_coordinate_normalization"] = "qt-fixed9-float32";
            report["reviewed_expected_matches"] = expected_reviewed_matches(reviewed_document);
            std::ofstream(artifacts / "review-expected.yaml") << YAML::Dump(report["reviewed_expected_matches"]);
            expected = editor->editedSet();
          } else {
            auto* selection = widget<QComboBox>(*editor, "matchEditorSelection");
            require(selection->count() >= 10, "Expected a useful automatic match set");
            selection->setCurrentIndex(0);
            auto* x = widget<QDoubleSpinBox>(*editor, "matchEditorCoordinate0");
            x->setValue(x->value() + 1.25);
            selection->setCurrentIndex(selection->count() - 1);
            widget<QPushButton>(*editor, "matchEditorDelete")->click();
            widget<QPushButton>(*editor, "matchEditorUndo")->click();
            widget<QPushButton>(*editor, "matchEditorRedo")->click();
            widget<QComboBox>(*editor, "matchEditorPair")->setCurrentIndex(1);
            selection->setCurrentIndex(selection->count() - 1);
            widget<QPushButton>(*editor, "matchEditorDelete")->click();
            expected = editor->editedSet();
            require(expected.frames.size() == 2, "Editor must preserve both camera pairs");
          }
          editor->grab().save(QString::fromStdString((artifacts / "editor.png").string()));
          before_manual = workspace_keys(game);
          manual_row = table->rowCount();
          click(*editor, "matchEditorSave");
          ++step;
        }
      } else if (
          step == 5 && table->rowCount() > manual_row &&
          widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton")->isEnabled()) {
        require(
            table->item(manual_row, 5)->text() == "Ready",
            "Manual calibration failed: " + table->item(manual_row, 5)->text().toStdString());
        require(table->item(automatic_row, 5)->text() == source_state, "Original source state changed");
        require(
            log->toPlainText().contains("automatic matching is bypassed"), "Real runner did not reuse manual input");
        require(
            image_sha256(source_workspace / "config.yaml") == source_config_sha,
            "Manual solve mutated its source config");
        const auto manual = added_record(game, before_manual, *table, manual_row);
        require(
            manual.state == "complete" && !manual.artifact_generation_id.empty() && manual.process_session_id == 0 &&
                manual.process_token.empty(),
            "Manual candidate lacks completed, retired native identity");
        manual_workspace = manual.workspace.game_directory;
        const auto manual_config = YAML::LoadFile((manual_workspace / "config.yaml").string());
        const auto fingerprint = hm::stitching::manual_control_point_fingerprint(manual_config);
        require(fingerprint.ok() && !fingerprint->empty(), "Manual solve has no saved manual fingerprint");
        const auto saved = hm::stitching::LoadCalibrationMatches(manual_workspace, *fingerprint);
        require(saved.ok() && saved->manual, "Manual solve did not retain its real edited snapshot");
        same_matches(*saved, expected);
        manual_fingerprint = QString::fromStdString(*fingerprint);
        report["manual_workspace"] = manual_workspace.string();
        report["manual_fingerprint"] = *fingerprint;
        report["artifact_generation"] = manual.artifact_generation_id;
        table->selectRow(manual_row);
        preview_output.clear();
        preview_incremental_output.clear();
        preview_log_offset = QFileInfo(QString::fromStdString((manual_workspace / "runner.log").string())).size();
        report["preview_log_start_offset"] = preview_log_offset;
        click(dialog, "previewStitchExperimentButton");
        preview_elapsed.start();
        ++step;
      } else if (step == 6) {
        require(preview_elapsed.elapsed() < 240000, "Current candidate preview never presented changing pixels");
        // The candidate log is append-only and belongs to this selected workspace.
        // Fence its starting offset before Play so previous ready markers cannot
        // authorize pixels. Retain the UI collector independently for diagnosis.
        QFile raw_log(QString::fromStdString((manual_workspace / "runner.log").string()));
        require(raw_log.open(QIODevice::ReadOnly), "Cannot read current candidate preview output");
        require(raw_log.size() >= preview_log_offset + preview_log_read, "Preview log was replaced or truncated");
        require(raw_log.seek(preview_log_offset + preview_log_read), "Cannot seek current preview output");
        const QByteArray added = raw_log.read(1024 * 1024);
        preview_log_read += added.size();
        preview_output = (preview_output + QString::fromUtf8(added)).right(1024 * 1024);
        report["preview_log_bytes_read"] = preview_log_read;
        report["preview_document_collected_chars"] = preview_incremental_output.size();
        report["preview_elapsed_ms"] = preview_elapsed.elapsed();
        const auto owner = current_record(game, manual_workspace);
        if (owner.process_session_id <= 0 || owner.process_token.empty()) {
          require(!preview_owner, "Preview retired before changing pixels were captured");
          return;
        }
        require(
            owner.state == "complete" &&
                owner.artifact_generation_id == report["artifact_generation"].as<std::string>(),
            "Preview candidate generation changed");
        if (!preview_process_matches(owner)) {
          // QProcess publishes the PID before the CLI's runtime re-exec has
          // settled. /proc/environ can be temporarily unavailable then. Never
          // accept pixels until ownership is observed, and never reacquire it
          // after an established owner disappears. The preview deadline above
          // also bounds a child that exits or never presents its token.
          require(!preview_owner, "Preview PID no longer has its exact native process token");
          report["preview_wait_reason"] = "Waiting for the initial native process token";
          return;
        }
        if (preview_owner)
          require(
              owner.process_session_id == preview_owner->process_session_id &&
                  owner.process_token == preview_owner->process_token,
              "Preview process identity changed during capture");
        else {
          preview_owner = owner;
          report["preview_session_id"] = owner.process_session_id;
          report["preview_process_token"] = owner.process_token;
        }
        const auto ready =
            QRegularExpression(
                "HSTREAM_PREVIEW channel=stitched status=ready generation=(\\d+) message=first GPU frame presented")
                .match(preview_output);
        report["preview_ready_marker_seen"] = ready.hasMatch();
        if (!ready.hasMatch()) {
          report["preview_wait_reason"] = "Current process has not emitted the ready marker";
          return;
        }
        if (preview_generation.isEmpty())
          preview_generation = ready.captured(1);
        require(ready.captured(1) == preview_generation, "Native renderer generation changed during capture");
        auto* video = widget<QWidget>(dialog, "stitchExperimentVideo");
        const QPoint global = video->mapToGlobal(QPoint());
        report["preview_widget_xid"] = static_cast<unsigned long long>(video->winId());
        report["preview_widget_visible"] = video->isVisible();
        report["preview_widget_geometry"] = QString("%1,%2 %3x%4")
                                                .arg(global.x())
                                                .arg(global.y())
                                                .arg(video->width())
                                                .arg(video->height())
                                                .toStdString();
        report["preview_screen"] = video->screen()->name().toStdString();
        report["preview_device_pixel_ratio"] = video->devicePixelRatioF();
        require(
            video->isVisible() && video->width() >= 300 && video->height() >= 150, "Preview surface is not visible");
        const auto identity = preview_process_identity(owner, std::filesystem::absolute(argv[4]), video->winId());
        if (preview_identity.isEmpty()) {
          preview_identity = identity;
          report["preview_process_start_ticks"] = identity.left(identity.indexOf('\0')).toStdString();
        }
        require(identity == preview_identity, "Preview starttime or command changed during capture");
        last_screen_capture = video->screen()->grabWindow(video->winId()).toImage();
        report["preview_screen_capture_available"] = !last_screen_capture.isNull();
        PixelSample screen_pixels;
        if (!last_screen_capture.isNull())
          screen_pixels = sample_pixels(last_screen_capture);
        report["preview_screen_last_nonblack_fraction"] = screen_pixels.nonblack_fraction;
        report["preview_screen_last_pixel_sha256"] = screen_pixels.sha256.toStdString();
        if (!diagnostic_capture_saved) {
          diagnostic_capture_saved = true;
          first_screen_capture_ms = preview_elapsed.elapsed();
          last_screen_capture.save(QString::fromStdString((artifacts / "preview-window-first-attempt.png").string()));
          dialog.grab().save(QString::fromStdString((artifacts / "preview-dialog-diagnostic.png").string()));
          video->screen()
              ->grabWindow(0, global.x(), global.y(), video->width(), video->height())
              .save(QString::fromStdString((artifacts / "preview-screen-region-diagnostic.png").string()));
        }
        QImage image = last_screen_capture;
        if (native_front_capture) {
          // Xwayland can return black XGetImage/QScreen pixels even while the
          // same Qt XID contains video in GL_FRONT. This explicit alternative
          // uses the existing one-shot renderer command, never a CPU video path.
          // Preserve screen evidence and keep compositor visibility unqualified.
          const QSize size = viewable_preview_size(video, report);
          require(
              static_cast<quint64>(size.width()) * size.height() * 4 <= 16 * 1024 * 1024,
              "Native preview exceeds the diagnostic readback budget");
          if (native_capture_size.isEmpty()) {
            native_capture_size = size;
            preview_window = video->winId();
          }
          require(size == native_capture_size && video->winId() == preview_window, "Native capture target changed");
          if (native_capture_path.isEmpty()) {
            if (preview_elapsed.elapsed() - first_screen_capture_ms < 300 ||
                (!first_pixel_hash.isEmpty() && preview_elapsed.elapsed() - first_pixel_ms < 500))
              return;
            require(native_capture_requests < 2, "Two bounded native captures did not prove changing pixels");
            native_capture_path = QString::fromStdString(
                (artifacts / ("preview-native-front-" + std::to_string(++native_capture_requests) + ".png")).string());
            request_front_buffer(
                owner, std::filesystem::absolute(argv[4]), preview_window, preview_identity, native_capture_path);
            native_capture_sent_ms = preview_elapsed.elapsed();
            report["preview_native_capture_requests"] = native_capture_requests;
            report["preview_wait_reason"] = "Waiting for exact-path native front-buffer identity acknowledgement";
            return;
          }
          const auto captured =
              QRegularExpression(
                  "runtime preview frame identity channel=stitched path=" +
                  QRegularExpression::escape(native_capture_path) + " generation=(\\d+) pipeline-pts-ns=(\\d+)")
                  .match(preview_output);
          if (!captured.hasMatch()) {
            require(preview_elapsed.elapsed() - native_capture_sent_ms < 5000, "Native capture identity timed out");
            return;
          }
          require(captured.captured(1) == preview_generation, "Native capture renderer generation changed");
          bool pts_valid = false;
          const auto pts = captured.captured(2).toULongLong(&pts_valid);
          require(pts_valid, "Native capture PTS is invalid");
          require(!first_capture_pts || pts > *first_capture_pts, "Native capture did not advance presentation PTS");
          require(
              preview_process_identity(owner, std::filesystem::absolute(argv[4]), preview_window) == preview_identity &&
                  viewable_preview_size(video, report) == native_capture_size,
              "Native capture acknowledgement no longer belongs to the same viewable target");
          const QFileInfo file(native_capture_path);
          require(
              file.isFile() && file.size() > 0 && file.size() <= 20 * 1024 * 1024,
              "Native PNG is unavailable or oversized");
          QImageReader reader(native_capture_path);
          const QSize dimensions = reader.size();
          require(
              dimensions == native_capture_size &&
                  static_cast<quint64>(dimensions.width()) * dimensions.height() * 4 <= 16 * 1024 * 1024,
              "Native PNG does not match the bounded Qt window");
          image = reader.read();
          YAML::Node capture;
          capture["path"] = native_capture_path.toStdString();
          capture["generation"] = preview_generation.toStdString();
          capture["pipeline_pts_ns"] = pts;
          capture["request_elapsed_ms"] = native_capture_sent_ms;
          capture["acknowledgement_elapsed_ms"] = preview_elapsed.elapsed();
          capture["png_sha256"] = image_sha256(native_capture_path.toStdString()).toStdString();
          report["preview_native_captures"].push_back(capture);
          if (!first_capture_pts)
            first_capture_pts = pts;
          native_capture_path.clear();
        }
        const auto pixels = sample_pixels(image);
        report["preview_last_nonblack_fraction"] = pixels.nonblack_fraction;
        report["preview_last_pixel_sha256"] = pixels.sha256.toStdString();
        if (pixels.nonblack_fraction < 0.05) {
          report["preview_wait_reason"] = "Owned video-window capture remains black";
          require(!native_front_capture, "Native front-buffer capture remains black");
          return;
        }
        if (first_pixel_hash.isEmpty()) {
          first_pixel_hash = pixels.sha256;
          first_pixel_ms = preview_elapsed.elapsed();
          first_pixel_size = image.size();
          preview_window = video->winId();
          report["preview_wait_reason"] = "Waiting for different nonblack pixels on the same current video window";
          require(
              image.save(QString::fromStdString((artifacts / "preview-first.png").string())),
              "Cannot save first preview capture");
        } else if (preview_elapsed.elapsed() - first_pixel_ms >= 500 && pixels.sha256 != first_pixel_hash) {
          require(
              image.size() == first_pixel_size && video->winId() == preview_window,
              "Preview geometry changed between samples");
          require(
              image.save(QString::fromStdString((artifacts / "preview-changing.png").string())),
              "Cannot save changing preview capture");
          write(artifacts / "preview-current-process.log", preview_output);
          report["preview_renderer_generation"] = preview_generation.toStdString();
          report["preview_first_sha256"] = first_pixel_hash.toStdString();
          report["preview_changed_sha256"] = pixels.sha256.toStdString();
          report["preview_nonblack_fraction"] = pixels.nonblack_fraction;
          report["preview_sample_separation_ms"] = preview_elapsed.elapsed() - first_pixel_ms;
          report["renderer_window_presentation_qualified"] = true;
          report["compositor_visibility_qualified"] = !native_front_capture;
          click(dialog, "stopStitchExperimentPreviewButton");
          ++step;
        }
      } else if (step == 7 && widget<QPushButton>(dialog, "applyStitchExperimentButton")->isEnabled()) {
        const auto stopped = current_record(game, manual_workspace);
        require(
            stopped.process_session_id == 0 && stopped.process_token.empty() && stopped.state == "complete",
            "Ordinary Stop did not retire native preview ownership");
        require(
            image_sha256(source_workspace / "config.yaml") == source_config_sha, "Preview changed its original source");
        report["ordinary_stop_retired"] = true;
        if (!retained_preview_report.empty()) {
          finish(
              0,
              "RETAINED PREVIEW ONLY: same manual artifact, changing owned pixels and ordinary Stop verified; no end-to-end rerun or promotion.\n");
          return;
        }
        if (!reviewed_path.empty() && !reviewed_promote) {
          finish(
              0,
              "CANDIDATE ONLY: genuine manual solve, current changing pixels and ordinary Stop verified; promotion awaits independent review.\n");
          return;
        }
        click(dialog, "applyStitchExperimentButton");
        ++step;
      } else if (step == 8 && applied) {
        const auto config = YAML::LoadFile((game / "config.yaml").string());
        const auto fingerprint = hm::stitching::manual_control_point_fingerprint(config);
        require(fingerprint.ok() && !fingerprint->empty(), "Promotion lost manual identity");
        auto saved = hm::stitching::LoadCalibrationMatches(game, *fingerprint);
        require(saved.ok(), saved.status().ToString());
        require(saved->frames.size() == expected.frames.size(), "Promotion lost pairs");
        size_t total = 0;
        for (size_t i = 0; i < expected.frames.size(); ++i) {
          require(
              saved->frames[i].matches.size() == expected.frames[i].matches.size(),
              "Saved edits were recapped/refilled");
          total += saved->frames[i].matches.size();
          for (size_t j = 0; j < expected.frames[i].matches.size(); ++j) {
            require(
                saved->frames[i].matches[j].left == expected.frames[i].matches[j].left &&
                    saved->frames[i].matches[j].right == expected.frames[i].matches[j].right,
                "Edited coordinates changed");
          }
        }
        std::ifstream project(game / "hm_project.pto");
        size_t project_control_points = 0;
        std::string line;
        while (std::getline(project, line))
          if (line.rfind("c ", 0) == 0)
            ++project_control_points;
        require(project_control_points == total, "Real Hugin project did not consume all edited matches");
        auto original = hm::stitching::LoadCalibrationMatches(game, saved->automatic_fingerprint);
        require(original.ok(), "Promoted original is unavailable for reset");
        same_matches(*saved, expected);
        require(QString::fromStdString(*fingerprint) == manual_fingerprint, "Promotion changed manual fingerprint");
        if (source_workspace != game) {
          require(
              image_sha256(source_workspace / "config.yaml") == source_config_sha,
              "Promotion changed the original candidate");
          require(
              current_record(game, source_workspace).state == report["source_state"].as<std::string>(),
              "Promotion changed the original candidate's terminal state");
        }
        saved_control_points = project_control_points;
        reopened_dialog = std::make_unique<StitchingExperimentDialog>(
            QString::fromStdString(game.string()),
            QString::fromLocal8Bit(argv[4]),
            QString::fromStdString(repo.string()),
            QString::fromStdString((repo / "configs/ds_hockey_app_config.yaml").string()),
            env,
            control_points,
            frame_count,
            argc == 6 ? QString::fromLocal8Bit(argv[5]) : QStringLiteral("00:09:42"));
        reopened_dialog->show();
        step = 10;
      } else if (step == 10) {
        auto* reopened_table = widget<QTableWidget>(*reopened_dialog, "stitchExperimentCandidates");
        int main_row = -1;
        for (int row = 0; row < reopened_table->rowCount(); ++row)
          if (reopened_table->item(row, 0)->text() == "Main calibration")
            main_row = row;
        require(
            main_row >= 0 && reopened_table->item(main_row, 5)->text() == "Ready",
            "Promoted Main did not reopen Ready");
        reopened_table->selectRow(main_row);
        click(*reopened_dialog, "inspectStitchExperimentFramesButton");
        step = 11;
      } else if (step == 11) {
        auto* viewer = reopened_dialog->findChild<QDialog*>("stitchExperimentFrameInspector");
        if (!viewer || !viewer->isVisible())
          return;
        click(*viewer, "stitchExperimentEditMatches");
        step = 12;
      } else if (step == 12) {
        auto* editor = dynamic_cast<MatchEditorDialog*>(reopened_dialog->findChild<QDialog*>("matchEditorDialog"));
        if (!editor || !editor->isVisible())
          return;
        const auto draft = editor->editedSet();
        same_matches(draft, expected);
        const auto reopened_config = YAML::LoadFile((game / "config.yaml").string());
        const auto reopened_snapshot =
            reopened_config["hstream_ui"]["stitching_calibration"]["match_snapshot"].as<std::string>();
        require(
            QString::fromStdString(reopened_snapshot) == manual_fingerprint,
            "Reopening changed the saved manual identity");
        const auto reopened = hm::stitching::LoadCalibrationMatches(game, reopened_snapshot);
        require(reopened.ok(), reopened.status().ToString());
        require(
            reopened->manual && reopened->fingerprint == reopened_snapshot,
            "Reopened saved calibration lost manual identity");
        // editedSet() deliberately returns an unpublished draft. The save
        // handler retains the immutable snapshot identity separately.
        require(
            draft.manual && draft.fingerprint.empty() && draft.input_fingerprint == reopened->input_fingerprint &&
                draft.automatic_fingerprint == reopened->automatic_fingerprint,
            "Reopened draft lost its saved input or automatic-match provenance");
        same_matches(draft, *reopened);
        report["ordinary_reopen_saved_fingerprint"] = reopened_snapshot;
        report["ordinary_reopen_draft_unpublished"] = true;
        editor->grab().save(QString::fromStdString((artifacts / "promoted-reopened-editor.png").string()));
        click(*editor, "matchEditorCancel");
        report["ordinary_reopen_verified"] = true;
        step = 13;
      } else if (step == 13) {
        if (reopened_dialog->findChild<QDialog*>("matchEditorDialog"))
          return;
        const auto config = YAML::LoadFile((game / "config.yaml").string());
        const auto framing = hm::stitching::read_stitch_projection_framing(config);
        const auto camera = hm::stitching::read_stitch_camera_selection(config);
        require(framing.ok(), framing.status().ToString());
        require(camera.ok(), camera.status().ToString());
        leveling = std::make_unique<RinkLevelingDialog>(
            QString::fromStdString(game.string()), framing->rotation_degrees, nullptr, *camera);
        require(leveling->loadError().isEmpty(), leveling->loadError().toStdString());
        for (const char* name : {"rinkLevelingCamera0", "rinkLevelingCamera1"})
          require(
              !static_cast<ScoreboardSelectionCanvas*>(widget<QWidget>(*leveling, name))->imageSize().isEmpty(),
              "Promoted manual calibration must load both leveling camera images");
        leveling->show();
        leveling->grab().save(QString::fromStdString((artifacts / "leveling-camera.png").string()));
        // Exercise a real Hugin preview with the saved angles, without changing
        // the promoted calibration. Post mode permits preview without new marks.
        widget<QCheckBox>(*leveling, "markRinkPostsCheck")->setChecked(true);
        click(*leveling, "previousRinkLevelingButton");
        preview_elapsed.restart();
        ++step;
      } else if (step == 14) {
        require(
            preview_elapsed.elapsed() < 120000,
            "Leveling preview did not finish: " +
                widget<QLabel>(*leveling, "rinkLevelingStatus")->text().toStdString());
        if (!widget<QPushButton>(*leveling, "acceptRinkLevelingButton")->isEnabled())
          return;
        require(
            !static_cast<ScoreboardSelectionCanvas*>(widget<QWidget>(*leveling, "rinkLevelingPreview"))
                 ->imageSize()
                 .isEmpty(),
            "Promoted manual calibration must render a leveling preview");
        leveling->grab().save(QString::fromStdString((artifacts / "leveling-preview.png").string()));
        require(
            RinkLevelingDialog::sourceRevision(QString::fromStdString(game.string())) == leveling->sourceRevision(),
            "Leveling preview changed the saved calibration");
        report["promoted"] = true;
        report["saved_control_points"] = saved_control_points;
        finish(
            0,
            QString(
                "PASS: %1 -> real manual solve -> current changing pixels -> ordinary Stop -> promotion -> ordinary editor reopen -> leveling preview; pairs=%2 points=%3.\n")
                .arg(source_mode)
                .arg(expected.frames.size())
                .arg(saved_control_points));
      }
    } catch (const std::exception& error) {
      std::cerr << "E2E step " << step << " failed: " << error.what() << '\n';
      write(artifacts / "failure.txt", QString::fromUtf8(error.what()));
      if (finishing) {
        report["cleanup_error"] = error.what();
        std::ofstream(artifacts / "workflow-report.yaml") << YAML::Dump(report);
        driver.stop();
        app.exit(1);
      } else {
        report["failure"] = error.what();
        finish(1, QString::fromUtf8(error.what()));
      }
    }
  });
  driver.start();
  return app.exec();
}
