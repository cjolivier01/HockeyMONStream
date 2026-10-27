#include "src/apps/hstream-ui/StitchingStillPreview.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/TransactionState.h"
#include "src/apps/hstream-ui/CalibrationImage.h"
#include "src/apps/hstream-ui/StitchingStillImage.h"

#include <tiffio.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>

#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

struct StitchingStillPreviewTestAccess {
  static void observe(StitchingStillPreview& preview, std::function<void()> metadata, std::function<void()> decoding) {
    preview.metadata_locked_observer_ = std::move(metadata);
    preview.decoding_observer_ = std::move(decoding);
  }
  static std::function<bool()> cancelled(const StitchingStillPreview& preview) {
    const auto state = preview.read_state_;
    return [state] { return state && state->cancelled.load(); };
  }
  static bool pending(const StitchingStillPreview& preview) {
    return preview.worker_ != nullptr;
  }
  static QPointer<QThread> worker(const StitchingStillPreview& preview) {
    return preview.worker_;
  }
};

namespace {
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
bool wait_for(const std::function<bool()>& ready) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 10000) {
    QApplication::processEvents();
    if (ready())
      return true;
    QThread::msleep(10);
  }
  return false;
}
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Cannot write fixture");
}
void tiff_fixture(const QString& path) {
  TIFF* file = TIFFOpen(QFile::encodeName(path).constData(), "w");
  require(file, "Cannot write TIFF fixture");
  TIFFSetField(file, TIFFTAG_IMAGEWIDTH, 4096);
  TIFFSetField(file, TIFFTAG_IMAGELENGTH, 64);
  TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, 16);
  std::vector<uint16_t> row(4096 * 3, 0);
  for (int x = 0; x < 4096; ++x)
    row[3 * x + 2] = 65535;
  for (int y = 0; y < 64; ++y)
    require(TIFFWriteScanline(file, row.data(), y) >= 0, "Cannot write TIFF row");
  TIFFClose(file);
}

SavedStitchingStill read_still(
    const QString& directory,
    const QByteArray& settings = {},
    const std::function<bool()>& cancelled = {},
    const std::function<void()>& decoding = {}) {
  std::mutex gate;
  return ReadSavedStitchingStill(
      directory, {}, settings, StitchingStillRevision(directory), gate, cancelled, {}, decoding);
}

void exercise_synchronous_reader() {
  QTemporaryDir game;
  const QByteArray complete = "hstream_ui: {stitching_calibration: {status: complete}}\n";
  write(game.filePath("config.yaml"), complete);
  QImage png(4096, 64, QImage::Format_RGB32);
  png.fill(Qt::red);
  require(png.save(game.filePath("s.png")), "Cannot save synchronous PNG fixture");
  const auto original = read_still(game.path());
  require(
      original.name == "s.png" && original.image.size() == QSize(2048, 32) &&
          original.image.pixelColor(0, 0) == QColor(Qt::red) && !original.retry,
      "The synchronous reader must accept and bound PNG-only legacy calibrations");
  require(original.revision == StitchingStillRevision(game.path()), "Still must bind its checked source revision");
  bool decoded_without_locks = false;
  const auto replaced = read_still(game.path(), {}, {}, [&] {
    auto writer = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
    decoded_without_locks = writer.ok() && bool(*writer);
    write(game.filePath("config.yaml"), complete + "changed: true\n");
  });
  require(decoded_without_locks, "Synchronous decoding must release publication locks");
  require(replaced.image.isNull() && replaced.retry, "A revision replaced during decoding must never be returned");
  write(game.filePath("config.yaml"), complete);
  bool cancelled = true;
  int decodes = 0;
  const auto before = read_still(game.path(), {}, [&] { return cancelled; }, [&] { ++decodes; });
  require(before.image.isNull() && decodes == 0, "Cancelled synchronous reads must not enter decoding");
  cancelled = false;
  const auto during = read_still(game.path(), {}, [&] { return cancelled; }, [&] { cancelled = true; });
  require(during.image.isNull() && !during.retry, "Cancellation during decoding must retire the bounded result");
  auto writer = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
  require(writer.ok() && *writer, "Cannot hold synchronous writer fixture");
  const auto busy = read_still(game.path());
  require(busy.image.isNull() && busy.retry, "An optional synchronous read must not wait for an active writer");
}

void exercise_metadata_cancellation() {
  QTemporaryDir game;
  write(game.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: complete}}\n");
  QImage image(32, 16, QImage::Format_RGB32);
  image.fill(Qt::red);
  require(image.save(game.filePath("s.png")), "Cannot save cancellation fixture");
  QWidget host;
  host.resize(80, 40);
  host.show();
  struct Phase {
    QSemaphore entered;
    QSemaphore release;
    void hold() {
      entered.release();
      release.acquire();
    }
  };
  auto metadata = std::make_shared<Phase>();
  StitchingStillPreview preview(&host);
  std::atomic<int> decodes{0};
  StitchingStillPreviewTestAccess::observe(preview, [metadata] { metadata->hold(); }, [&] { ++decodes; });
  preview.setSource(game.path());
  const bool entered = wait_for([&] { return metadata->entered.tryAcquire(); });
  if (!entered) {
    metadata->release.release();
    preview.setSource({});
    require(false, "Reader did not enter the actual locked metadata section");
  }
  const auto cancelled = StitchingStillPreviewTestAccess::cancelled(preview);
  bool owned_lock_at_cancel = false;
  std::thread release_metadata([&] {
    QElapsedTimer deadline;
    deadline.start();
    while (!cancelled() && deadline.elapsed() < 10000)
      std::this_thread::yield();
    if (cancelled()) {
      auto other = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
      owned_lock_at_cancel = other.ok() && !*other;
    }
    metadata->release.release();
  });
  QElapsedTimer drain;
  drain.start();
  preview.setSource({});
  const auto drain_ns = drain.nsecsElapsed();
  release_metadata.join();
  require(owned_lock_at_cancel, "Cancellation must be exercised while this reader actually owns the artifact lock");
  auto after = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
  require(after.ok() && *after, "Clearing the source must release all reader artifact/config locks before returning");
  after->reset();
  require(
      wait_for([&] { return !StitchingStillPreviewTestAccess::pending(preview); }), "Cancelled reader did not finish");
  require(decodes == 0 && preview.isHidden(), "Cancelled metadata must not start decoding or paint a late still");
  std::cout << "Own metadata cancellation drain: " << drain_ns << " ns\n";

  // Hold a genuine decode after its metadata locks have gone. Both source
  // cancellation and widget destruction must return while decoding is held.
  auto decoding = std::make_shared<Phase>();
  auto retired = std::make_unique<StitchingStillPreview>(&host);
  StitchingStillPreviewTestAccess::observe(*retired, {}, [decoding] { decoding->hold(); });
  retired->setSource(game.path());
  const bool decode_entered = wait_for([&] { return decoding->entered.tryAcquire(); });
  if (!decode_entered) {
    decoding->release.release();
    retired.reset();
    require(false, "Reader did not enter its lock-free decode phase");
  }
  const auto worker = StitchingStillPreviewTestAccess::worker(*retired);
  std::promise<void> cleared;
  auto completed = cleared.get_future();
  bool waited_for_decode = false;
  std::thread watchdog([&] {
    if (completed.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
      waited_for_decode = true;
      decoding->release.release();
    }
  });
  drain.restart();
  retired->setSource({});
  retired.reset();
  const auto clear_ns = drain.nsecsElapsed();
  cleared.set_value();
  watchdog.join();
  const bool decode_still_running = worker && worker->isRunning();
  decoding->release.release();
  require(!waited_for_decode && decode_still_running, "Clearing/destroying a still must never wait for image decoding");
  require(wait_for([&] { return worker.isNull(); }), "Retired worker did not clean up safely after widget destruction");
  std::cout << "Clear/destruction during held decode: " << clear_ns << " ns\n";

  // An independent calibration owner still wins. The cancellation gate must
  // not unlock, borrow or wait on that owner's artifact transaction.
  auto writer = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
  require(writer.ok() && *writer, "Cannot hold external writer fixture");
  StitchingStillPreviewTestAccess::observe(preview, {}, {});
  preview.setSource(game.path());
  require(
      wait_for([&] { return !StitchingStillPreviewTestAccess::pending(preview); }),
      "Reader waited for external writer");
  preview.setSource({});
  require(preview.isHidden(), "Busy external generation must never display a still");
  auto blocked = hm::stitching::try_lock_canvas_constraint_artifacts(game.path().toStdString());
  require(blocked.ok() && !*blocked, "Cancelling a reader must preserve external writer ownership");
  writer->reset();
}

void exercise_pending_recovery() {
  for (const auto kind :
       {hm::stitching::TransactionJournalKind::kStitch, hm::stitching::TransactionJournalKind::kRink}) {
    for (const bool legacy : {false, true}) {
      QTemporaryDir game;
      const QByteArray config = "hstream_ui: {stitching_calibration: {status: complete}}\n";
      write(game.filePath("config.yaml"), config);
      QImage image(32, 16, QImage::Format_RGB32);
      image.fill(Qt::red);
      require(image.save(game.filePath("s.png")), "Cannot save pending-recovery fixture");
      const QString prefix = kind == hm::stitching::TransactionJournalKind::kStitch ? "stitch" : "rink";
      const QString transaction = (legacy ? ".hstream-" : "hstream-") + prefix + "-Ab12CD";
      require(QDir(game.path()).mkdir(transaction), "Cannot create transaction fixture");
      const auto journal = QDir(game.filePath(transaction));
      write(journal.filePath("journal_version"), "2\n");
      write(journal.filePath("state"), "PREPARED\n");
      write(journal.filePath("rollback-payload"), "must remain unopened and unchanged\n");
      if (!legacy) {
        require(
            hm::stitching::mark_transaction_recovery_pending(game.path().toStdString(), kind).ok(),
            "Cannot mark pending recovery");
      }
      auto advisory = hm::stitching::try_lock_canvas_constraint_snapshot(game.path().toStdString());
      require(advisory.ok() && !*advisory, "Advisory reader must decline pending/legacy journals without recovery");
      QWidget host;
      host.resize(80, 40);
      host.show();
      StitchingStillPreview preview(&host);
      std::atomic<int> metadata_reads{0}, decodes{0};
      StitchingStillPreviewTestAccess::observe(preview, [&] { ++metadata_reads; }, [&] { ++decodes; });
      preview.setSource(game.path());
      require(
          wait_for([&] { return !StitchingStillPreviewTestAccess::pending(preview); }),
          "Advisory reader did not retire");
      preview.setSource({});
      require(
          metadata_reads == 0 && decodes == 0 && preview.isHidden(), "Pending recovery must not enter metadata/decode");
      QFile saved_config(game.filePath("config.yaml")), saved_state(journal.filePath("state"));
      require(
          saved_config.open(QIODevice::ReadOnly) && saved_config.readAll() == config,
          "Optional still must not recover or alter the calibration config");
      require(
          saved_state.open(QIODevice::ReadOnly) && saved_state.readAll() == "PREPARED\n" &&
              QFile::exists(journal.filePath("rollback-payload")),
          "Optional still must leave journal and rollback payload for explicit recovery");
    }
  }
}

void exercise_advisory_generation() {
  QTemporaryDir game;
  const auto path = game.path().toStdString();
  write(game.filePath("config.yaml"), "hstream_ui: {stitching_calibration: {status: complete}}\n");
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
    write(game.filePath(name), "metadata fixture\n");
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"})
    tiff_fixture(game.filePath(name));
  QImage image(32, 16, QImage::Format_RGB32);
  image.fill(Qt::red);
  require(
      image.save(game.filePath("seam_file.png")) && image.save(game.filePath("s.png")),
      "Cannot save advisory identity fixture");
  auto artifact_lock = hm::stitching::try_lock_canvas_constraint_snapshot(path);
  require(artifact_lock.ok() && *artifact_lock, "Cannot hold advisory identity fixture");
  const auto legacy = hm::stitching::stitch_artifact_snapshot_generation_id_locked(path);
  require(
      legacy.ok() && !QFile::exists(game.filePath("stitching_generation_id")),
      "Legacy stat identity must remain read-only");
  const auto published = hm::stitching::stitch_artifact_generation_id_locked(path);
  require(published.ok(), "Cannot publish current identity fixture");
  const auto current = hm::stitching::stitch_artifact_snapshot_generation_id_locked(path);
  require(current.ok() && *current == *published, "Advisory identity must accept current metadata-bound generations");
  QFile identity_file(game.filePath("stitching_generation_id"));
  require(identity_file.open(QIODevice::ReadOnly), "Cannot read current identity fixture");
  const QByteArray identity = identity_file.readAll();
  identity_file.close();
  write(game.filePath("autooptimiser_out.pto"), "changed metadata fixture\n");
  const auto changed = hm::stitching::stitch_artifact_snapshot_generation_id_locked(path);
  require(
      !changed.ok() && identity_file.open(QIODevice::ReadOnly) && identity_file.readAll() == identity,
      "Advisory identity must decline changed bindings without republishing them");
  identity_file.close();
  const auto bindings = hm::stitching::stitch_artifact_binding_revision_locked(path);
  require(bindings.ok(), "Cannot read current fixture bindings");
  const QByteArray version2 = "version=2\nlogical-size=" + QByteArray::number(published->size()) +
      "\nbindings-size=" + QByteArray::number(bindings->size()) + '\n' + QByteArray::fromStdString(*published) +
      QByteArray::fromStdString(*bindings);
  write(game.filePath("stitching_generation_id"), version2);
  const auto older = hm::stitching::stitch_artifact_snapshot_generation_id_locked(path);
  require(!older.ok(), "Advisory reader must leave older identity upgrades to explicit validation");
  artifact_lock->reset();
  QWidget host;
  host.resize(80, 40);
  host.show();
  StitchingStillPreview preview(&host);
  std::atomic<int> decodes{0};
  StitchingStillPreviewTestAccess::observe(preview, {}, [&] { ++decodes; });
  preview.setSource(game.path(), QString::fromStdString(*published));
  require(
      wait_for([&] { return !StitchingStillPreviewTestAccess::pending(preview); }),
      "Older identity reader did not retire");
  preview.setSource({});
  require(
      decodes == 0 && preview.isHidden() && identity_file.open(QIODevice::ReadOnly) &&
          identity_file.readAll() == version2,
      "Optional still must not upgrade an older identity or start image decoding");
}

void exercise() {
  QTemporaryDir game;
  const QByteArray complete = "hstream_ui:\n  stitching_calibration:\n    status: complete\n";
  write(game.filePath("config.yaml"), complete);
  QImage png(4096, 64, QImage::Format_RGB32);
  png.fill(Qt::red);
  require(png.save(game.filePath("s.png")), "Cannot save PNG");
  tiff_fixture(game.filePath("panorama.tif"));
  const auto proxy = readCalibrationTiff(game.filePath("panorama.tif"), 2048);
  require(
      proxy.size() == QSize(2048, 32) && proxy.pixelColor(0, 0) == QColor(Qt::blue),
      "16-bit TIFF must become a bounded color-correct preview");
  const auto png_proxy = readCalibrationPng(game.filePath("s.png"), QSize(2048, 32));
  require(
      png_proxy.size() == QSize(2048, 32) && png_proxy.pixelColor(0, 0) == QColor(Qt::red),
      "PNG must become a bounded color-correct preview");
  QWidget host;
  host.resize(800, 400);
  host.show();
  StitchingStillPreview preview(&host);
  preview.setSource(game.path());
  require(wait_for([&] { return preview.isVisible(); }), "Complete calibration must display a still");
  require(preview.property("calibrationImage").toString() == "s.png", "Prefer PNG over TIFF");
  host.resize(900, 500);
  QApplication::processEvents();
  require(preview.size() == host.size(), "Still must follow preview area resizing");
  write(game.filePath("config.yaml"), complete + "    stale_from: canvas\n");
  require(wait_for([&] { return preview.isHidden(); }), "Invalidated calibration must clear the still");
  write(game.filePath("config.yaml"), complete);
  require(wait_for([&] { return preview.isVisible(); }), "New complete calibration must restore the still");
  QFile::remove(game.filePath("s.png"));
  require(
      wait_for([&] { return preview.property("calibrationImage").toString() == "panorama.tif"; }),
      "Missing PNG must fall back to TIFF");
  write(game.filePath("s.png"), "broken image");
  require(
      wait_for([&] { return preview.property("calibrationImage").toString() == "panorama.tif"; }),
      "Corrupt PNG must fall back to TIFF");
  preview.setSource({});
  require(preview.isHidden() && !preview.property("calibrationImage").isValid(), "Play must clear immediately");
  // Clear a request before its completion callback can paint anything.
  preview.setSource(game.path());
  preview.setSource({});
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 1200) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  require(preview.isHidden(), "Late decode must not reappear after Play");
  preview.setSource(game.path(), "a different artifact generation");
  timer.restart();
  while (timer.elapsed() < 1200) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  require(preview.isHidden(), "Changed candidate generation must not display a still");
  preview.setSource(game.path());
  require(wait_for([&] { return preview.isVisible(); }), "Stop must restore the selected still");
  write(game.filePath("config.yaml"), "invalid: [yaml");
  require(wait_for([&] { return preview.isHidden(); }), "Malformed settings must fail closed");
  auto config = YAML::Load(complete.toStdString());
  hm::stitching::StitchingBackendChoices choices;
  choices.control_point_matcher = "superpoint-lightglue";
  choices.mapping_backend = "nona";
  choices.projection = "cylindrical";
  choices.run_autooptimizer = true;
  config["stitching"]["control_point_matcher"] = choices.control_point_matcher;
  config["stitching"]["mapping_backend"] = choices.mapping_backend;
  config["stitching"]["projection"] = choices.projection;
  config["stitching"]["run_autooptimizer"] = choices.run_autooptimizer;
  hm::stitching::write_stitch_camera_selection(config, choices.camera);
  hm::stitching::write_stitch_projection_framing(config, choices.projection_framing);
  config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "preview-generation";
  require(
      hm::stitching::reserve_stitching_backend_generation_in_config(config, "preview-generation", choices).ok(),
      "Cannot prepare generation claim");
  const auto claimed = QByteArray::fromStdString(YAML::Dump(config));
  write(game.filePath("config.yaml"), claimed);
  const auto isolated_home = qgetenv("HOME");
  qunsetenv("HOME");
  require(
      !read_still(game.path()).image.isNull(), "An explicit saved game must remain readable without a user HOME layer");
  qputenv("HOME", isolated_home);
  require(wait_for([&] { return preview.isVisible(); }), "Matching generation claim must allow the still");
  config["stitching"]["projection"] = "rectilinear";
  write(game.filePath("config.yaml"), QByteArray::fromStdString(YAML::Dump(config)));
  require(wait_for([&] { return preview.isHidden(); }), "Editing settings must invalidate an old generation claim");
  write(game.filePath("config.yaml"), claimed);
  preview.setSource({});
  require(
      read_still(game.path(), "stitching: {projection: rectilinear}").image.isNull(),
      "Synchronous readers must reject changed frozen effective settings too");
  preview.setSource(game.path(), {}, "stitching: {projection: rectilinear}");
  timer.restart();
  while (timer.elapsed() < 1200) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  require(preview.isHidden(), "Changed effective UI defaults must reject an old saved calibration");
  preview.setSource({});
  require(
      !read_still(game.path(), "stitching: {projection: cylindrical}").image.isNull(),
      "Synchronous readers must accept the same matching parameter claim as Qt");
  preview.setSource(game.path(), {}, "stitching: {projection: cylindrical}");
  require(wait_for([&] { return preview.isVisible(); }), "Matching effective UI choices must restore the still");
  // A completed canvas is usable before the downstream PNG/mask has been rebuilt.
  config = YAML::Load(claimed.toStdString());
  config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
  require(png.save(game.filePath("s.png")), "Cannot restore PNG fixture");
  write(game.filePath("config.yaml"), QByteArray::fromStdString(YAML::Dump(config)));
  require(
      wait_for([&] { return preview.property("calibrationImage").toString() == "panorama.tif"; }),
      "A completed solve must skip its invalidated PNG and use the new panorama");
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir user_home;
  qputenv("HOME", QFile::encodeName(user_home.path()));
  try {
    exercise_synchronous_reader();
    exercise_metadata_cancellation();
    exercise_pending_recovery();
    exercise_advisory_generation();
    exercise();
    if (argc == 2) {
      QWidget host;
      host.resize(1200, 500);
      host.show();
      StitchingStillPreview preview(&host);
      preview.setSource(QString::fromLocal8Bit(argv[1]));
      require(wait_for([&] { return preview.isVisible(); }), "Real saved calibration must load");
      require(preview.grab().save("/tmp/hstream-stitching-still.png"), "Cannot save preview capture");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
