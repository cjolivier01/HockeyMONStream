#include "hstream/src/libs/publication/Publication.h"
#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
struct HStreamWindowTestAccess {
  static bool clean(HStreamWindow& window, const fs::path& runner, const QProcessEnvironment& env) {
    return window.runStitchingClean(
        QString::fromStdString(runner.string()),
        QString::fromStdString(runner.parent_path().string()),
        env,
        false,
        {});
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
    const auto clean_helper = path / "clean-helper.sh";
    std::ofstream(clean_helper)
        << "#!/bin/sh\n"
           "test -z \"${HSTREAM_JOB_MANIFEST+x}\" || exit 21\n"
           "test -z \"${HSTREAM_ARCHIVE_RUN_ID+x}\" || exit 22\n"
           "test \"$HSTREAM_RUNTIME_CACHE_DIR\" = \"$EXPECTED_RUNTIME_CACHE\" || exit 23\n";
    fs::permissions(clean_helper, fs::perms::owner_all);
    auto clean_env = QProcessEnvironment::systemEnvironment();
    clean_env.insert("HSTREAM_JOB_MANIFEST", "/reserved/archive/run.yaml");
    clean_env.insert("HSTREAM_ARCHIVE_RUN_ID", "reserved-archive-run");
    clean_env.insert("HSTREAM_RUNTIME_CACHE_DIR", "/reserved/archive/runtime");
    clean_env.insert("EXPECTED_RUNTIME_CACHE", "/reserved/archive/runtime");
    require(HStreamWindowTestAccess::clean(window, clean_helper, clean_env), "clean helper cannot close the archive run");
    require(clean_env.contains("HSTREAM_JOB_MANIFEST"), "archive launch environment retains its manifest");
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
      require(!fs::exists(hm::publication::ReceiptPath(game, id)), "managed failure has not started publication");
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
      for (const QString route : {QString("archive-program-4k"), QString("archive-stitched")})
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
