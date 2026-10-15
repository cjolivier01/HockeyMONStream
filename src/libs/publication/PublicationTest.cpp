#include "hstream/src/libs/publication/Publication.h"
#include "hstream/src/libs/recording/Database.h"

#include <sys/stat.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace pub = hm::publication;
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void produce(const fs::path& fixture, bool broken_database, bool calibration = false) {
  const fs::path work = pub::EnvironmentWorkingDirectory();
  pub::RequireTelemetry(true);
  if (calibration) {
    const auto old = work / "calibration_telemetry.db";
    std::ofstream(old) << "incomplete preparatory database";
    pub::RegisterOutput(old, "telemetry");
    pub::RetireOutput(old);
  }
  for (const char* kind : {"program", "program-4k", "stitched"}) {
    const auto path = work / (std::string(kind) + ".mkv");
    std::ofstream(path.string() + ".hstream-owner-lock").close();
    fs::copy_file(fixture, path);
    pub::RegisterOutput(path, kind);
  }
  const auto path = work / "game_telemetry-1.db";
  {
    std::ofstream create(path);
    create.close();
    hm::recording::Database db(path.string(), true);
    db.Exec(hm::recording::Schema());
    db.Exec("INSERT INTO runs VALUES('test-run','game','today','test','{}','{}',1,'end-of-stream',1)");
    if (broken_database)
      db.Exec("UPDATE runs SET completed=0");
  }
  pub::RegisterOutput(path, "telemetry");
  std::ofstream(work / "auxiliary.log") << "Owned scratch log\n";
  pub::CompletePipeline(true, 1234);
}
size_t count(const fs::path& game, const std::string& extension) {
  size_t result = 0;
  for (const auto& entry : fs::directory_iterator(game))
    result += entry.path().extension() == extension;
  return result;
}
int main(int argc, char** argv) {
  try {
    if (argc >= 3 && std::string(argv[1]) == "--produce") {
      produce(
          argv[2],
          argc > 3 && std::string(argv[3]) == "--broken-db",
          argc > 3 && std::string(argv[3]) == "--calibration");
      return argc > 3 && std::string(argv[3]) == "--fail-after-completion" ? 42 : 0;
    }
    const fs::path base = fs::path(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") /
        ("publication-test-" + std::to_string(getpid()));
    fs::create_directories(base);
    const auto fixture = base / "fixture.mkv";
    // A real encoded input checks stream-copy remuxing and the MP4 atom layout.
    const std::string make_fixture =
        "ffmpeg -hide_banner -loglevel error -f lavfi -i color=size=64x64:rate=5 "
        "-t 0.4 -c:v libx264 -y '" +
        fixture.string() + "'";
    require(std::system(make_fixture.c_str()) == 0, "create real encoded test fixture");
    const std::vector<std::string> command = {fs::canonical(argv[0]).string(), "--produce", fixture.string()};
    const auto exercise = [&](const std::string& name, const fs::path& work_root, const char* interruption) {
      const auto game = base / name / "game";
      fs::create_directories(game);
      const std::string id = "run-test-" + name;
      if (interruption)
        setenv("HSTREAM_TEST_PUBLICATION_STOP", interruption, 1);
      bool failed = false;
      try {
        require(pub::RunJob(game, work_root, id, command) == 0, "successful runner");
      } catch (const std::exception& error) {
        failed = true;
        std::cerr << name << ": " << error.what() << "\n";
      }
      unsetenv("HSTREAM_TEST_PUBLICATION_STOP");
      require(failed == bool(interruption), "expected interruption: " + name);
      require(pub::RunJob(game, work_root, id, command) == 0, "resume interrupted transaction");
      require(pub::RunJob(game, work_root, id, {"/bin/false"}) == 0, "completed retry must not encode again");
      require(!fs::exists(work_root / "game/runs" / id), "working run directory removed");
      require(
          count(game, ".mp4") == 3 && count(game, ".db") == 1 && count(game, ".json") == 3,
          "all outputs published once");
      for (const auto& entry : fs::directory_iterator(game)) {
        require(!entry.is_directory(), "no game staging directory remains");
        require(entry.path().extension() != ".next", "no journal temporary file remains");
        require(entry.path().extension() != ".hstream-pin", "no video reservation remains");
        if (entry.path().extension() == ".mp4") {
          std::ifstream input(entry.path(), std::ios::binary);
          std::string bytes((std::istreambuf_iterator<char>(input)), {});
          require(bytes.find("moov") < bytes.find("mdat"), "MP4 has faststart");
        }
        if (entry.path().extension() == ".json")
          require(YAML::LoadFile(entry.path().string())["start_time_ms"].as<int>() == 1234, "archive origin preserved");
      }
      const auto receipt = YAML::LoadFile(pub::ReceiptPath(game, id).string());
      require(receipt["state"].as<std::string>() == "complete", "receipt records completed cleanup");
    };
    exercise("success", base / "work-success", nullptr);
    {
      const auto game = base / "calibration/game";
      fs::create_directories(game);
      auto runner = command;
      runner.push_back("--calibration");
      require(
          pub::RunJob(game, base / "calibration/work", "run-calibration", runner) == 0,
          "retired calibration DB is scratch, not a required completed recording");
      require(count(game, ".db") == 1, "only final telemetry generation is published");
    }
    for (const bool failed_exit : {false, true}) {
      const auto game = base / (failed_exit ? "failed-exit/game" : "lost-supervisor/game");
      const auto root = game.parent_path() / "work";
      const std::string id = "run-supervisor-outcome";
      fs::create_directories(game);
      auto runner = command;
      if (failed_exit)
        runner.push_back("--fail-after-completion");
      else
        setenv("HSTREAM_TEST_PUBLICATION_STOP", "after-runner-exit", 1);
      try {
        require(pub::RunJob(game, root, id, runner) == 42, "failed runner exit preserved");
      } catch (const std::exception&) {
        require(!failed_exit, "only lost supervisor throws on first execution");
      }
      unsetenv("HSTREAM_TEST_PUBLICATION_STOP");
      bool refused = false;
      try {
        pub::RunJob(game, root, id, command);
      } catch (const std::exception&) {
        refused = true;
      }
      require(refused && count(game, ".mp4") == 0, "retry requires recorded runner success");
      const auto manifest = root / "game/runs" / id / "run.yaml";
      refused = false;
      try {
        pub::Finalize(manifest);
      } catch (const std::exception&) {
        refused = true;
      }
      require(refused && fs::exists(manifest.parent_path() / "program.mkv"), "direct resume preserves failed run");
    }
    for (const char* point : {"after-prepare", "after-link", "after-publish", "during-cleanup", "after-cleanup"})
      exercise(point, base / (std::string("work-") + point), point);
    if (fs::is_directory("/dev/shm")) {
      const auto other_work = fs::path("/dev/shm") / ("hstream-publication-" + std::to_string(getpid()));
      exercise("cross-filesystem", other_work, "after-link");
      fs::remove_all(other_work);
    }
    const auto game = base / "foreign/game";
    const auto work_root = base / "foreign/work";
    const std::string id = "run-test-foreign";
    fs::create_directories(game);
    setenv("HSTREAM_TEST_PUBLICATION_STOP", "after-publish", 1);
    try {
      pub::RunJob(game, work_root, id, command);
    } catch (const std::exception&) {
    }
    unsetenv("HSTREAM_TEST_PUBLICATION_STOP");
    const auto work = work_root / "game/runs" / id;
    std::ofstream(work / "not-ours") << "must survive";
    bool refused = false;
    try {
      pub::RunJob(game, work_root, id, command);
    } catch (const std::exception&) {
      refused = true;
    }
    require(refused && fs::exists(work / "not-ours"), "unknown working entries block cleanup");
    fs::remove(work / "not-ours");
    require(pub::RunJob(game, work_root, id, command) == 0, "retry cleanup after obstruction is removed");

    {
      const auto metadata_game = base / "metadata/game";
      const auto metadata_root = base / "metadata/work";
      const auto metadata_work = metadata_root / "game/runs/run-metadata";
      fs::create_directories(metadata_game);
      setenv("HSTREAM_TEST_PUBLICATION_STOP", "after-publish", 1);
      try {
        pub::RunJob(metadata_game, metadata_root, "run-metadata", command);
      } catch (const std::exception&) {
      }
      unsetenv("HSTREAM_TEST_PUBLICATION_STOP");
      fs::rename(metadata_work / "run.yaml", base / "original-run.yaml");
      std::ofstream(metadata_work / "run.yaml") << "FOREIGN";
      refused = false;
      try {
        pub::Finalize(pub::ReceiptPath(metadata_game, "run-metadata"));
      } catch (const std::exception&) {
        refused = true;
      }
      require(refused && fs::file_size(metadata_work / "run.yaml") == 7, "foreign protocol metadata survives");
      fs::remove(metadata_work / "run.yaml");
      fs::rename(base / "original-run.yaml", metadata_work / "run.yaml");
      pub::Finalize(pub::ReceiptPath(metadata_game, "run-metadata"));
      require(!fs::exists(metadata_work), "metadata cleanup resumes after original restored");
    }
    {
      const auto retry_game = base / "retry-launch/game";
      const auto retry_root = base / "retry-launch/work";
      fs::create_directories(retry_game);
      for (int attempt = 0; attempt < 3; ++attempt) {
        const auto stage = base / ("launch-" + std::to_string(attempt));
        fs::create_directory(stage);
        fs::permissions(stage, fs::perms::owner_all);
        std::ofstream(stage / "staged-library") << "owned launch data";
        setenv("HSTREAM_JOB_PLUGIN_STAGE", stage.c_str(), 1);
        if (attempt < 2)
          setenv("HSTREAM_FFMPEG", "/bin/false", 1);
        else
          unsetenv("HSTREAM_FFMPEG");
        bool failure = false;
        try {
          pub::RunJob(retry_game, retry_root, "run-retry-launch", command);
        } catch (const std::exception&) {
          failure = true;
        }
        require(failure == (attempt < 2), "publication retry outcome");
      }
      unsetenv("HSTREAM_JOB_PLUGIN_STAGE");
      for (int attempt = 0; attempt < 3; ++attempt)
        require(!fs::exists(base / ("launch-" + std::to_string(attempt))), "all attempts' launch scratch removed");
    }

    const auto competing_game = base / "competing/game";
    const auto competing_work = base / "competing/work";
    fs::create_directories(competing_game);
    setenv("HSTREAM_TEST_PUBLICATION_STOP", "after-prepare", 1);
    try {
      pub::RunJob(competing_game, competing_work, "run-first-reservation", command);
    } catch (const std::exception&) {
    }
    unsetenv("HSTREAM_TEST_PUBLICATION_STOP");
    require(
        pub::RunJob(competing_game, competing_work, "run-second-reservation", command) == 0,
        "another run can publish while the first is interrupted");
    require(
        pub::RunJob(competing_game, competing_work, "run-first-reservation", command) == 0,
        "reserved generation survives another run's publication");
    require(count(competing_game, ".mp4") == 6, "competing runs keep distinct generations");

    const auto replaced_game = base / "replaced/game";
    const auto replaced_work_root = base / "replaced/work";
    const auto replaced_work = replaced_work_root / "game/runs/run-replaced-source";
    fs::create_directories(replaced_game);
    setenv("HSTREAM_TEST_PUBLICATION_STOP", "after-publish", 1);
    try {
      pub::RunJob(replaced_game, replaced_work_root, "run-replaced-source", command);
    } catch (const std::exception&) {
    }
    unsetenv("HSTREAM_TEST_PUBLICATION_STOP");
    const auto saved_source = base / "saved-source.mkv";
    fs::rename(replaced_work / "program.mkv", saved_source);
    std::ofstream(replaced_work / "program.mkv") << "foreign replacement";
    refused = false;
    try {
      pub::RunJob(replaced_game, replaced_work_root, "run-replaced-source", command);
    } catch (const std::exception&) {
      refused = true;
    }
    require(
        refused && fs::file_size(replaced_work / "program.mkv") == 19, "replacement source survives cleanup refusal");
    fs::remove(replaced_work / "program.mkv");
    fs::rename(saved_source, replaced_work / "program.mkv");
    require(
        pub::RunJob(replaced_game, replaced_work_root, "run-replaced-source", command) == 0,
        "restored source allows cleanup retry");

    const auto bad_game = base / "bad-db/game";
    fs::create_directories(bad_game);
    auto bad_command = command;
    bad_command.push_back("--broken-db");
    refused = false;
    try {
      pub::RunJob(bad_game, base / "bad-work", "run-bad-database", bad_command);
    } catch (const std::exception&) {
      refused = true;
    }
    require(
        refused && fs::exists(base / "bad-work/game/runs/run-bad-database/program.mkv"),
        "telemetry failure must preserve every working video");
    fs::remove_all(base);
    std::cout << "Publication tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
