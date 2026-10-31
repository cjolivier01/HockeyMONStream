#include "hstream/src/libs/publication/Publication.h"
#include "hstream/src/libs/recording/Database.h"
#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;
struct HStreamWindowTestAccess {
  static bool stopChoice(
      HStreamWindow& window,
      const fs::path& helper,
      const fs::path& result,
      bool publish,
      bool hockeymon_video) {
    window.pipeline_process_->start(
        "/usr/bin/python3", {QString::fromStdString(helper.string()), QString::fromStdString(result.string())});
    if (!window.pipeline_process_->waitForStarted())
      return false;
    QTest::qWait(100);
    window.active_hockeymon_program_ = hockeymon_video;
    window.active_backend_output_kinds_ = hockeymon_video ? QStringList{"archive-file"} : QStringList{};
    window.active_run_telemetry_requested_ = !hockeymon_video;
    window.preview_runtime_ready_ = true;
    window.calibration_pending_ = false;
    bool checked_default = false;
    QTimer::singleShot(0, [&] {
      auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      checked_default = dialog->defaultButton() && dialog->defaultButton()->text() == "Don't publish";
      if (const auto path = qEnvironmentVariable("HSTREAM_STOP_SCREENSHOT"); !path.isEmpty())
        dialog->grab().save(path);
      if (publish) {
        for (auto* button : dialog->buttons())
          if (button->text() == "Publish partial outputs")
            button->click();
      } else {
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(dialog, &escape);
      }
    });
    window.confirmStopPipeline();
    return checked_default && window.pipeline_process_->state() == QProcess::NotRunning;
  }
  static bool clean(HStreamWindow& window, const fs::path& runner, const QProcessEnvironment& env) {
    return window.runStitchingClean(
        QString::fromStdString(runner.string()), QString::fromStdString(runner.parent_path().string()), env, false, {});
  }
  static void publish(
      HStreamWindow& window,
      const fs::path& manifest,
      const std::vector<std::pair<fs::path, std::string>>& outputs) {
    window.managed_archive_manifest_ = QString::fromStdString(manifest.string());
    for (const auto& [source, kind] : outputs) {
      const QString id = kind == "program-4k" ? "archive-program-4k"
          : kind == "stitched"                ? "archive-stitched"
                                              : "archive-file";
      window.output_states_.at(id)->setText("FINALIZING");
      window.pending_archive_finalizations_.push_back(
          {QString::fromStdString(source.string()), "game", id, false, kind == "stitched"});
    }
    window.startNextArchiveFinalization();
  }
  static void openLog(HStreamWindow& window, const fs::path& source) {
    const QString path = QString::fromStdString(source.string());
    window.beginArchiveJobLog(path, "test-log");
    window.resolveArchiveJobLogPath(path);
    window.appendLog("managed test log");
  }
  static void finish(
      HStreamWindow& window,
      const fs::path& manifest,
      const fs::path& source,
      bool telemetry_only,
      int exit_code,
      QProcess::ExitStatus exit_status,
      bool stopped_by_user,
      bool publish_partial = false) {
    window.managed_archive_manifest_ = QString::fromStdString(manifest.string());
    window.active_run_game_id_ = "game";
    window.active_run_telemetry_requested_ = telemetry_only;
    window.pipeline_stop_requested_ = stopped_by_user;
    window.pipeline_publish_partial_ = publish_partial;
    window.calibration_pending_ = false;
    window.archive_finalize_failed_ = false;
    if (!telemetry_only) {
      window.active_archive_output_path_ = QString::fromStdString(source.string());
      window.active_archive_initial_size_ = -1;
      window.active_archive_initial_mtime_ms_ = -1;
    }
    window.handlePipelineFinished(exit_code, exit_status);
  }
  static bool busy(const HStreamWindow& window) {
    return window.telemetry_publication_worker_ != nullptr;
  }
  static bool failed(const HStreamWindow& window) {
    return window.archive_finalize_failed_;
  }
  static QString state(const HStreamWindow& window, const QString& id) {
    return window.output_states_.at(id)->text();
  }
  static QString detail(const HStreamWindow& window) {
    return window.archive_finalize_detail_->text();
  }
};
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string readFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "read retained file: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
size_t countFiles(const fs::path& directory, const std::string& extension) {
  size_t count = 0;
  for (const auto& entry : fs::directory_iterator(directory))
    count += entry.path().extension() == extension;
  return count;
}
int main(int argc, char** argv) {
  QTemporaryDir root;
  if (!root.isValid())
    return 1;
  const auto path = fs::path(root.path().toStdString());
  qputenv("HOME", root.path().toUtf8());
  qputenv("HM_GAME_DIR", (root.path() + "/games").toUtf8());
  qputenv("HM_OUTPUT_WORK_DIR", (root.path() + "/work").toUtf8());
  QApplication application(argc, argv);
  try {
    const auto game = path / "games/game's files";
    fs::create_directories(game);
    std::ofstream(game / "config.yaml") << "{}\n";
    const auto fixture = path / "fixture.mkv";
    require(
        QProcess::execute(
            "ffmpeg",
            {"-hide_banner",
             "-loglevel",
             "error",
             "-f",
             "lavfi",
             "-i",
             "color=size=64x64:rate=5",
             "-t",
             "0.4",
             "-c:v",
             "libx264",
             "-y",
             QString::fromStdString(fixture.string())}) == 0,
        "create fixture");
    HStreamWindow window;
    const auto stop_helper = path / "stop-helper.py";
    std::ofstream(stop_helper) << "import signal,sys\n"
                                  "def stopped(*args):\n"
                                  " open(sys.argv[1], 'w').write('interrupted')\n"
                                  " sys.exit(0)\n"
                                  "signal.signal(signal.SIGINT, stopped)\n"
                                  "print('ready', flush=True)\n"
                                  "line=sys.stdin.readline().strip()\n"
                                  "open(sys.argv[1], 'w').write(line)\n";
    for (const bool hockeymon_video : {true, false})
      for (const bool publish : {false, true}) {
        const auto result = path / (publish ? "publish-stop" : "discard-stop");
        require(
            HStreamWindowTestAccess::stopChoice(window, stop_helper, result, publish, hockeymon_video),
            "Stop must ask and default to no publication for telemetry and either backend's videos");
        require(
            readFile(result) == (publish ? "q" : "interrupted"), "Only explicit consent may send a publishing stop");
      }
    const auto clean_helper = path / "clean-helper.sh";
    std::ofstream(clean_helper) << "#!/bin/sh\n"
                                   "test -z \"${HSTREAM_JOB_MANIFEST+x}\" || exit 21\n"
                                   "test -z \"${HSTREAM_ARCHIVE_RUN_ID+x}\" || exit 22\n"
                                   "test \"$HSTREAM_RUNTIME_CACHE_DIR\" = \"$EXPECTED_RUNTIME_CACHE\" || exit 23\n";
    fs::permissions(clean_helper, fs::perms::owner_all);
    auto clean_env = QProcessEnvironment::systemEnvironment();
    clean_env.insert("HSTREAM_JOB_MANIFEST", "/reserved/archive/run.yaml");
    clean_env.insert("HSTREAM_ARCHIVE_RUN_ID", "reserved-archive-run");
    clean_env.insert("HSTREAM_RUNTIME_CACHE_DIR", "/reserved/archive/runtime");
    clean_env.insert("EXPECTED_RUNTIME_CACHE", "/reserved/archive/runtime");
    require(
        HStreamWindowTestAccess::clean(window, clean_helper, clean_env), "clean helper cannot close the archive run");
    require(clean_env.contains("HSTREAM_JOB_MANIFEST"), "archive launch environment retains its manifest");
    for (const bool telemetry_only : {true, false}) {
      for (const std::string outcome : {"nonzero", "crash", "graceful-stop", "stop-without-publication"}) {
        const bool success = outcome == "graceful-stop";
        const std::string id = std::string("ui-finished-") + (telemetry_only ? "telemetry-" : "video-") + outcome;
        const auto finished_game = path / id / "game";
        fs::create_directories(finished_game);
        const auto manifest = hm::publication::CreateRun(finished_game, path / "work", id);
        hm::publication::BeginRun(manifest, telemetry_only, telemetry_only);
        setenv("HSTREAM_JOB_MANIFEST", manifest.c_str(), 1);
        const auto source = manifest.parent_path() / (telemetry_only ? "game_telemetry-1.db" : "program.mkv");
        if (telemetry_only) {
          std::ofstream(source).close();
          hm::recording::Database database(source.string(), true);
          database.Exec(hm::recording::Schema());
          database.Exec("INSERT INTO runs VALUES('test-run','game','today','test','{}','{}',1,'end-of-stream',1)");
        } else {
          fs::copy_file(fixture, source);
        }
        hm::publication::RegisterOutput(source, telemetry_only ? "telemetry" : "program");
        HStreamWindowTestAccess::openLog(window, source);
        const auto log = source.string() + ".log";
        const auto guard = log + ".hstream-pin";
        require(fs::exists(log) && fs::equivalent(log, guard), id + ": log identity is pinned");
        hm::publication::CompletePipeline(true, 2000);
        unsetenv("HSTREAM_JOB_MANIFEST");
        const auto receipt = hm::publication::ReceiptPath(finished_game, id);
        const auto manifest_before = readFile(manifest);
        const auto receipt_before = readFile(receipt);
        const auto source_before = readFile(source);
        const auto log_before = readFile(log);
        const auto log_offset = window.logText().size();
        require(hm::publication::PipelineCompleted(manifest), id + ": runner encoded its output before exiting");
        HStreamWindowTestAccess::finish(
            window,
            manifest,
            source,
            telemetry_only,
            outcome == "nonzero" ? 42 : 0,
            outcome == "crash" ? QProcess::CrashExit : QProcess::NormalExit,
            success || outcome == "stop-without-publication",
            success);
        if (!success) {
          require(!HStreamWindowTestAccess::busy(window), id + ": failed runner cannot start publication");
          require(
              countFiles(finished_game, ".db") == 0 && countFiles(finished_game, ".mp4") == 0,
              id + ": failed runner publishes no output");
          require(
              readFile(manifest) == manifest_before && readFile(receipt) == receipt_before &&
                  readFile(source) == source_before,
              id + ": failure preserves encoded manifest, admission receipt and source bytes");
          require(
              fs::equivalent(log, guard) && readFile(log).find(log_before) == 0,
              id + ": failure retains log content and its identity guard");
          require(
              window.logText()
                  .mid(log_offset)
                  .contains("Managed outputs not published: pipeline did not complete successfully"),
              id + ": failure explains why working outputs were retained");
          if (!telemetry_only)
            require(
                HStreamWindowTestAccess::state(window, "archive-file") == "INCOMPLETE",
                id + ": failed video remains incomplete");
        } else {
          QElapsedTimer timeout;
          timeout.start();
          while (HStreamWindowTestAccess::busy(window) && timeout.elapsed() < 15000)
            QTest::qWait(20);
          require(
              !HStreamWindowTestAccess::busy(window) && !HStreamWindowTestAccess::failed(window),
              id + ": graceful Stop completes publication");
          require(
              YAML::LoadFile(receipt.string())["state"].as<std::string>() == "complete" &&
                  countFiles(finished_game, ".db") == (telemetry_only ? 1 : 0) &&
                  countFiles(finished_game, ".mp4") == (telemetry_only ? 0 : 1),
              id + ": graceful Stop publishes exactly the requested output");
          require(
              !fs::exists(manifest.parent_path()) && !fs::exists(log) && !fs::exists(guard),
              id + ": successful publication removes workspace and log identities");
        }
      }
    }
    for (const bool failed : {false, true}) {
      const std::string id = failed ? "ui-explicitly-failed" : "ui-incomplete-managed";
      const auto manifest = hm::publication::CreateRun(game, path / "work", id);
      hm::publication::BeginRun(manifest);
      setenv("HSTREAM_JOB_MANIFEST", manifest.c_str(), 1);
      const auto source = manifest.parent_path() / "program.mkv";
      fs::copy_file(fixture, source);
      hm::publication::RegisterOutput(source, "program");
      if (failed)
        hm::publication::CompletePipeline(false, 0);
      unsetenv("HSTREAM_JOB_MANIFEST");
      HStreamWindowTestAccess::publish(window, manifest, {{source, "program"}});
      require(HStreamWindowTestAccess::failed(window), "known managed failure cannot use legacy publication");
      require(HStreamWindowTestAccess::state(window, "archive-file") == "ERROR", "managed failure marks output error");
      require(fs::exists(source), "managed failure retains working video");
      require(
          YAML::LoadFile(hm::publication::ReceiptPath(game, id).string())["state"].as<std::string>() == "admitted",
          "managed failure retains its admission binding without starting publication");
    }
    for (const std::string scenario : {"success", "remux-failure", "cleanup-failure"}) {
      const bool fail = scenario != "success";
      const std::string id = "ui-managed-" + scenario;
      const auto manifest = hm::publication::CreateRun(game, path / "work", id);
      hm::publication::BeginRun(manifest);
      setenv("HSTREAM_JOB_MANIFEST", manifest.c_str(), 1);
      const auto external = path / "custom-output" / scenario;
      fs::create_directories(external);
      const std::vector<std::pair<fs::path, std::string>> outputs = {
          {external / "program-4k.mkv", "program-4k"}, {manifest.parent_path() / "stitched.mkv", "stitched"}};
      for (const auto& [source, kind] : outputs) {
        fs::copy_file(fixture, source);
        hm::publication::RegisterOutput(source, kind);
      }
      HStreamWindowTestAccess::openLog(window, outputs.front().first);
      const auto log = outputs.front().first.string() + ".log";
      require(fs::exists(log), "custom archive log created outside private workspace");
      hm::publication::CompletePipeline(true, 2000);
      unsetenv("HSTREAM_JOB_MANIFEST");
      if (scenario == "remux-failure")
        qputenv("HSTREAM_FFMPEG", "/bin/false");
      if (scenario == "cleanup-failure")
        qputenv("HSTREAM_TEST_PUBLICATION_STOP", "after-cleanup");
      const QString untouched = HStreamWindowTestAccess::state(window, "archive-file");
      HStreamWindowTestAccess::publish(window, manifest, outputs);
      QElapsedTimer timeout;
      timeout.start();
      while (HStreamWindowTestAccess::busy(window) && timeout.elapsed() < 15000)
        QTest::qWait(20);
      require(!HStreamWindowTestAccess::busy(window), "background publication completed");
      require(HStreamWindowTestAccess::failed(window) == fail, "UI reports publication outcome");
      require(HStreamWindowTestAccess::state(window, "archive-file") == untouched, "unselected route is unchanged");
      for (const QString& route : {QString("archive-program-4k"), QString("archive-stitched")})
        require(
            HStreamWindowTestAccess::state(window, route) == (fail ? "ERROR" : "SAVED"),
            "every selected route reports outcome");
      if (fail) {
        if (scenario == "remux-failure")
          require(fs::exists(outputs.front().first) && fs::exists(log), "failed remux retains external video and log");
        else
          require(!fs::exists(manifest), "cleanup interruption removed original manifest");
        const auto receipt = hm::publication::ReceiptPath(game, id);
        QString quoted = QString::fromStdString(receipt.string());
        quoted.replace("'", "'\\''");
        require(
            HStreamWindowTestAccess::detail(window).contains("--manifest '" + quoted + "'"),
            "failure offers a quoted durable receipt command");
        qunsetenv("HSTREAM_FFMPEG");
        qunsetenv("HSTREAM_TEST_PUBLICATION_STOP");
        hm::publication::Finalize(receipt);
      }
      require(!fs::exists(manifest.parent_path()), "UI publication leaves no working run directory");
      require(!fs::exists(log), "publication removes external custom archive log");
      require(!fs::exists(log + ".hstream-pin"), "publication removes external log identity guard");
      require(fs::exists(hm::publication::ReceiptPath(game, id)), "durable result receipt retained");
    }
    std::cout << "Managed UI publication tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
