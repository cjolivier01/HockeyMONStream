#include "hstream/src/libs/publication/Publication.h"
#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
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
  static void publish(HStreamWindow& window, const fs::path& manifest, const fs::path& source) {
    window.managed_archive_manifest_ = QString::fromStdString(manifest.string());
    window.pending_archive_finalizations_.push_back(
        {QString::fromStdString(source.string()), "game", "archive-file", false, false});
    window.startNextArchiveFinalization();
  }
  static bool busy(const HStreamWindow& window) {
    return window.telemetry_publication_worker_ != nullptr;
  }
  static bool failed(const HStreamWindow& window) {
    return window.archive_finalize_failed_;
  }
  static QString state(const HStreamWindow& window) {
    return window.output_states_.at("archive-file")->text();
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
    const auto game = path / "games/game";
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
    for (const bool fail : {false, true}) {
      const std::string id = fail ? "ui-managed-failure" : "ui-managed-success";
      const auto manifest = hm::publication::CreateRun(game, path / "work", id);
      hm::publication::BeginRun(manifest);
      setenv("HSTREAM_JOB_MANIFEST", manifest.c_str(), 1);
      const auto source = manifest.parent_path() / "program.mkv";
      fs::copy_file(fixture, source);
      hm::publication::RegisterOutput(source, "program");
      hm::publication::CompletePipeline(true, 2000);
      unsetenv("HSTREAM_JOB_MANIFEST");
      if (fail)
        qputenv("HSTREAM_FFMPEG", "/bin/false");
      HStreamWindowTestAccess::publish(window, manifest, source);
      QElapsedTimer timeout;
      timeout.start();
      while (HStreamWindowTestAccess::busy(window) && timeout.elapsed() < 15000)
        QTest::qWait(20);
      require(!HStreamWindowTestAccess::busy(window), "background publication completed");
      require(HStreamWindowTestAccess::failed(window) == fail, "UI reports publication outcome");
      if (fail) {
        require(fs::exists(source), "failed remux retains working source");
        qunsetenv("HSTREAM_FFMPEG");
        hm::publication::Finalize(hm::publication::ReceiptPath(game, id));
      } else {
        require(HStreamWindowTestAccess::state(window) == "SAVED", "UI marks completed Program saved");
      }
      require(!fs::exists(manifest.parent_path()), "UI publication leaves no working run directory");
      require(fs::exists(hm::publication::ReceiptPath(game, id)), "durable result receipt retained");
    }
    std::cout << "Managed UI publication tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
