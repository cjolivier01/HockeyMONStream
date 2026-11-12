#include "src/apps/hstream-ui/FreshCalibrationEvidence.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <iostream>
#include <stdexcept>
#include "hstream/src/libs/common/GameOperationOwner.h"

namespace evidence = fresh_calibration_evidence;
namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
bool wait(const std::function<bool()>& condition) {
  QElapsedTimer deadline;
  deadline.start();
  while (deadline.elapsed() < 10000) {
    QCoreApplication::processEvents();
    if (condition())
      return true;
    QThread::msleep(5);
  }
  return false;
}
std::string claim(const std::filesystem::path& game) {
  std::filesystem::create_directories(game);
  const auto token = hm::game_operation::NewToken();
  require(token.ok(), "create token");
  auto owner = hm::game_operation::LocalOwner("experiment", *token, *token);
  {
    const auto lock = hm::game_operation::Lock::Acquire(game);
    require(lock.ok() && (*lock)->Claim(owner).ok(), "claim owner");
  }
  hm::game_operation::RegisterAuthorization(game, *token);
  require(hm::game_operation::EnableSupervisedChildProof(*token).ok(), "enable proof");
  return *token;
}
void put(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write CPU fixture");
}
} // namespace
int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  try {
    require(root.isValid(), "temporary root");
    const auto config = root.filePath("config.yaml");
    put(config, "original\n");
    require(evidence::snapshot(root.filePath("absent")).absent(), "absence is explicit");
    require(QFile::link(config, root.filePath("link")), "make symlink fixture");
    require(!evidence::snapshot(root.filePath("link")).valid(), "reject leaf symlink");
    require(!evidence::snapshot(config, 2).valid(), "enforce byte bound");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto first_hash = evidence::fingerprint(config, 1024, deadline);
    require(
        evidence::same_content(first_hash, evidence::snapshot(config).metadata),
        "streamed hash matches pinned snapshot");
    put(config, "changed\n");
    require(
        !evidence::same_content(first_hash, evidence::fingerprint(config, 1024, deadline)),
        "changed bytes cannot match");
    require(
        evidence::fingerprint(config, 1024, std::chrono::steady_clock::now())["state"] != "captured",
        "expired hash deadline fails");
    int clock_reads = 0;
    const auto late = evidence::fingerprint(config, 1024, deadline, [&] {
      return ++clock_reads == 1 ? deadline - std::chrono::nanoseconds(1) : deadline + std::chrono::nanoseconds(1);
    });
    require(clock_reads == 2 && late["state"] != "captured", "last chunk finishing past the deadline cannot pass");
    put(root.filePath("empty"), {});
    require(
        evidence::fingerprint(root.filePath("empty"), 1024, std::chrono::steady_clock::now())["state"] != "captured",
        "empty files cannot bypass an expired shared deadline");
    for (bool failed_start : {false, true}) {
      put(config, "original\n");
      const std::filesystem::path game = root.filePath(failed_start ? "failed-start" : "success").toStdString();
      const auto token = claim(game);
      QProcess process;
      // The observer is installed before Prepare, as in the real UI.
      evidence::Observer observer(
          process, config, [&] { return QJsonObject{{"token", QString::fromStdString(token)}}; });
      bool finished = false, released = false, observed_before_release = false;
      // The production finished connection predates the adapter/observer's exit;
      // stateChanged must preserve proof before this releases/unlinks the owner.
      QObject::connect(
          &process,
          qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          &application,
          [&](int code, QProcess::ExitStatus status) {
            observed_before_release = observer.verified;
            const auto stopped = hm::ui::SupervisedProcessStopped(process);
            released = stopped.ok() && *stopped && hm::game_operation::ReleaseLocal(game, token, true).ok();
            finished = code == 0 && status == QProcess::NormalExit;
          });
      process.setProgram("/usr/bin/python3");
      const QStringList child{"-c", "import sys; open(sys.argv[1], 'w').write('mutated\\n')", config};
      process.setArguments(child);
      require(hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true).ok(), "prepare real guard");
      if (failed_start)
        process.setProgram(root.filePath("no-such-executable"));
      process.start();
      require(wait([&] { return process.state() == QProcess::NotRunning; }), "bounded process completion");
      if (failed_start) {
        require(!observer.started && !observer.verified, "unlaunched permit cannot pass observer");
        require(hm::game_operation::ReleaseLocal(game, token, true).ok(), "release cancelled unlaunched permit");
      } else {
        require(
            finished && released && observed_before_release, "validated receipt captured before production release");
        require(
            observer.admitted.bytes == "original\n" && evidence::snapshot(config).bytes == "mutated\n",
            "Starting snapshot must precede worker mutation");
        require(observer.commandUnchanged(), "actual command matches admission");
        auto changed = process.arguments();
        changed.append("unexpected");
        process.setArguments(changed);
        require(!observer.commandUnchanged(), "changed actual command cannot match admission");
        auto expected = child;
        expected.prepend("/usr/bin/python3");
        require(
            observer.launch["child_argv"].toArray() == QJsonArray::fromStringList(expected),
            "actual child argv retained");
        require(!QFileInfo::exists(observer.launch["receipt"].toString()), "production validator removed receipt");
        require(observer.retirement["receipt"].toObject()["state"] == "captured", "pinned receipt survived unlink");
        require(evidence::write(root.filePath("proof.json"), observer.retirement), "bounded evidence write");
      }
      const auto owner = hm::game_operation::Inspect(game);
      require(owner.ok() && !owner->has_value(), "no leaked CPU operation owner");
    }
    {
      QProcess unsupervised;
      evidence::Observer observer(unsupervised, config, [] { return QJsonObject{}; });
      const auto arbitrary = root.filePath("arbitrary-C");
      put(arbitrary, "C");
      unsupervised.start(
          "/usr/bin/true", {"--receipt", arbitrary, "--token", "other", "--nonce", "other", "--", "/usr/bin/true"});
      require(
          wait([&] { return unsupervised.state() == QProcess::NotRunning; }) && !observer.verified &&
              !observer.retirement["validator_ok"].toBool(),
          "main exit without supervised proof cannot pass");
    }
    {
      const std::filesystem::path game = root.filePath("missed-observer").toStdString();
      const auto token = claim(game);
      QProcess process;
      process.setProgram("/usr/bin/true");
      require(
          hm::ui::PrepareSupervisedProcess(process, game, token, 500, 10, true).ok(),
          "prepare missed-observer fixture");
      process.start(); // Deliberately miss synchronous Starting, without touching the real receipt.
      evidence::Observer observer(process, config, [] { return QJsonObject{}; });
      require(wait([&] { return process.state() == QProcess::NotRunning; }), "missed observer process completed");
      require(
          !observer.verified && observer.launches == 0, "later valid proof cannot repair missed admission evidence");
      const auto proof = hm::ui::SupervisedProcessStopped(process);
      require(
          proof.ok() && *proof && hm::game_operation::ReleaseLocal(game, token, true).ok(),
          "genuine missed-observer cleanup");
    }
    QProcessEnvironment env;
    require(evidence::flags(env).isEmpty(), "legacy mode unchanged");
    env.insert("HSTREAM_UI_E2E_ACTIVE_SAVE", "1");
    require(!evidence::flags(env).isEmpty(), "active save alone rejected");
    env.remove("HSTREAM_UI_E2E_ACTIVE_SAVE");
    for (const char* key :
         {"HSTREAM_UI_E2E_FRESH_UNSAVED",
          "HSTREAM_UI_E2E_FRESH_CALIBRATION",
          "HSTREAM_UI_E2E_CALIBRATION_COMPLETION_ONLY"})
      env.insert(key, "1");
    env.insert("HSTREAM_UI_E2E_RUN_MODE", "stitch-calibration");
    env.insert("HSTREAM_UI_E2E_REFERENCE_MS", "5000");
    env.insert("HSTREAM_UI_E2E_TIMEOUT_MS", "4500000");
    env.insert("HSTREAM_UI_E2E_GAME_ID", "fresh-game");
    env.insert("HM_GAME_DIR", root.path());
    env.insert("HSTREAM_UI_E2E_ARTIFACT_DIR", root.filePath("evidence"));
    env.insert("HSTREAM_UI_E2E_FRESH_LEFT", config);
    put(root.filePath("right.mp4"), "right source");
    env.insert("HSTREAM_UI_E2E_FRESH_RIGHT", root.filePath("right.mp4"));
    require(evidence::flags(env).isEmpty(), "valid unsaved flags");
    for (const auto& pair :
         {std::pair{"HSTREAM_UI_E2E_PREPARE_GAME", "1"},
          {"HSTREAM_UI_E2E_CLEAN_CALIBRATION", "1"},
          {"HSTREAM_UI_E2E_FRESH_UNSAVED", "0"},
          {"HSTREAM_UI_E2E_TIMEOUT_MS", "0"},
          {"HSTREAM_UI_E2E_REFERENCE_MS", "-5"},
          {"HSTREAM_UI_E2E_RUN_MODE", "program"},
          {"HSTREAM_UI_E2E_RINK_MAP_ONLY", "1"},
          {"HSTREAM_UI_TEST_TELEMETRY_RECEIPT_ONLY", "1"},
          {"HSTREAM_UI_E2E_GAME_ID", "../escape"}}) {
      auto invalid = env;
      invalid.insert(pair.first, pair.second);
      require(!evidence::flags(invalid).isEmpty(), "incompatible flags rejected");
    }
    env.insert("HSTREAM_UI_E2E_ACTIVE_SAVE", "1");
    require(evidence::flags(env).isEmpty(), "valid active save variant");
    std::cout
        << "Fresh calibration evidence CPU checks passed: bounded files, real guard, launch timing, retirement ordering, missing proof, launch failure, flags\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
