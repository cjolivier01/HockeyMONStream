#include <yaml-cpp/yaml.h>
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/recording/Database.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {
// Older UI recipes also contain preview/network sinks. Jobs always need a file
// encoder; an empty archive selection resolves the shared export defaults.
std::vector<std::string> encode_job_arguments(const YAML::Node& saved_args, const std::string& requested_sinks) {
  std::vector<std::string> result, encoders;
  bool calibration_only = false;
  auto select = [&encoders](const std::string& list) {
    std::istringstream stream(list);
    std::string sink;
    while (std::getline(stream, sink, ',')) {
      if (sink == "ENCODE_FILE" || sink == "ENCODE_STITCHED_FILE" || sink == "ENCODE_PROGRAM_4K_FILE") {
        if (std::find(encoders.begin(), encoders.end(), sink) == encoders.end())
          encoders.push_back(sink);
      }
    }
  };
  for (size_t i = 0; saved_args.IsSequence() && i < saved_args.size(); ++i) {
    const std::string arg = saved_args[i].as<std::string>();
    if (arg == "--stitching-calibration-only") {
      calibration_only = true;
    } else if (arg == "--enable-sinks" || arg == "-k") {
      if (++i == saved_args.size())
        throw std::runtime_error("saved --enable-sinks is missing its value");
      select(saved_args[i].as<std::string>());
    } else if (arg.rfind("--enable-sinks=", 0) == 0) {
      select(arg.substr(std::string("--enable-sinks=").size()));
    } else if (arg == "--show" || arg == "--render-realtime" || arg == "--encode-job") {
      continue;
    } else if (arg == "--show-scaled" || arg == "--show-stitching" || arg == "--show-playtracker") {
      if (++i == saved_args.size())
        throw std::runtime_error("saved preview option is missing its value");
    } else if (arg.rfind("--show-", 0) != 0 && arg.rfind("--ui-preview", 0) != 0) {
      result.push_back(arg);
    }
  }
  if (!requested_sinks.empty()) {
    encoders.clear();
    select(requested_sinks);
    if (encoders.empty())
      throw std::runtime_error("--enable-sinks must select at least one file encoder");
  }
  if (encoders.empty()) {
    const auto baseline = hm::baseline_config::load();
    if (!baseline.ok())
      throw std::runtime_error(baseline.status().ToString());
    const auto defaults = baseline->values["hstream_ui"]["export_archive_routes"];
    if (!defaults.IsSequence() || defaults.size() == 0)
      throw std::runtime_error("Baseline hstream_ui.export_archive_routes must select an archive");
    for (const auto& route : defaults) {
      const auto name = route.as<std::string>();
      if (name == "program")
        select("ENCODE_FILE");
      else if (name == "program_4k")
        select("ENCODE_PROGRAM_4K_FILE");
      else if (name == "stitched")
        select("ENCODE_STITCHED_FILE");
      else
        throw std::runtime_error("Baseline hstream_ui.export_archive_routes contains an unsupported archive");
    }
  }
  const bool stitched_only = encoders.size() == 1 && encoders.front() == "ENCODE_STITCHED_FILE";
  if (calibration_only && !stitched_only)
    throw std::runtime_error(
        "Stitching Calibration jobs require only Archive Stitched; use Program mode for Program archives");
  if (stitched_only)
    result.push_back("--stitching-calibration-only");
  std::string sinks = "--enable-sinks=";
  for (const auto& encoder : encoders) {
    if (sinks.back() != '=')
      sinks += ',';
    sinks += encoder;
  }
  result.push_back(sinks);
  result.push_back("--encode-job");
  return result;
}

void validate_interactive_publication(const std::vector<std::string>& args) {
  bool calibration = false, archive = false, program = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--stitching-calibration-only")
      calibration = true;
    std::string sinks;
    if (arg == "--enable-sinks" || arg == "-k") {
      if (++i == args.size())
        throw std::runtime_error("saved --enable-sinks is missing its value");
      sinks = args[i];
    } else if (arg.rfind("--enable-sinks=", 0) == 0) {
      sinks = arg.substr(std::string("--enable-sinks=").size());
    }
    std::istringstream stream(sinks);
    std::string sink;
    while (std::getline(stream, sink, ',')) {
      const bool program_sink = sink == "ENCODE_FILE" || sink == "ENCODE_PROGRAM_4K_FILE";
      archive = archive || program_sink || sink == "ENCODE_STITCHED_FILE";
      program = program || program_sink;
    }
  }
  if (!archive)
    throw std::runtime_error("--interactive-publication requires a selected archive encoder");
  if (calibration && program)
    throw std::runtime_error("Stitching Calibration sessions require only Archive Stitched");
}

std::string quote(const std::string& value) {
  std::string result = "'";
  for (char c : value)
    result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
}

std::string environment(const char* key) {
  const char* value = std::getenv(key);
  return value ? value : "";
}

void write_script(const fs::path& output, const std::string& contents, bool overwrite) {
  if (!overwrite && fs::exists(output))
    throw std::runtime_error("output already exists; use --force to replace it: " + output.string());
  std::string pattern = output.string() + ".XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back(0);
  int fd = mkstemp(name.data());
  if (fd < 0)
    throw std::runtime_error("cannot create script in " + output.parent_path().string());
  close(fd);
  const fs::path temporary(name.data());
  try {
    std::ofstream stream(temporary, std::ios::binary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream << contents;
    stream.close();
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
    if (overwrite) {
      fs::rename(temporary, output);
    } else {
      // Publish without a check/rename race that could replace another job.
      fs::create_hard_link(temporary, output);
      fs::remove(temporary);
    }
  } catch (...) {
    fs::remove(temporary);
    throw;
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    fs::path game, output, config, working;
    std::string runner = "hstream-cli", finalizer = "hstream-finalize";
    std::string requested_sinks;
    std::vector<std::string> directives;
    bool overwrite = false, interactive = false, interactive_publication = false, runtime_only = false,
         calibration_prompts = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg(argv[i]);
      if (arg == "--help" || arg == "-h") {
        std::cout << "Usage: hstream-job --game-dir DIR [--output FILE] [--force] [--interactive]\n"
                     "  [--sbatch '--partition=gpu'] [--sbatch '--time=02:00:00']\n"
                     "  [--runner COMMAND] [--finalizer COMMAND] [--config FILE] [--working-directory DIR]\n"
                     "  [--enable-sinks ENCODE_FILE,ENCODE_STITCHED_FILE,ENCODE_PROGRAM_4K_FILE]\n"
                     "Save an executable job (default: DIR/hstream-job.sh). Never launches the job.\n"
                     "Commands default to hstream-cli and hstream-finalize on the execution host PATH.\n"
                     "Config and runtime discovery belong to that host; no build paths are saved.\n"
                     "SBATCH defaults: one node, one task, one GPU. Additional directives override defaults.\n"
                     "Reads config.yaml and hstream_ui.job.arguments saved by the UI. Without a saved\n"
                     "job or selected file encoder, defaults to the 4K Program MP4. Rendering is disabled.\n"
                     "DriveGPT capture defaults on; the UI's explicit checkbox choice is preserved.\n"
                     "Successful jobs publish faststart MP4s/databases to the game and clean their working files.\n"
                     "--interactive preserves the saved sinks and stdin, executes the same resolved runner,\n"
                     "and runs without archive publication. Its supervisor must set a private\n"
                     "HSTREAM_RUNTIME_CACHE_DIR and clean it after every descendant has stopped.\n"
                     "Extra script arguments are appended as literal runner arguments in interactive mode.\n";
        std::cout << "--calibration-prompts preserves explicitly configured rink-leveling and crop flows\n"
                     "for managed jobs; the supervisor must service calibration prompts.\n";
        std::cout << "--runtime-only forwards only the script's arguments, for native sampling and\n"
                     "runtime-info subcommands that cannot inherit a saved playback recipe.\n";
        std::cout
            << "--interactive-publication preserves interactive sinks and stdin through the managed finalizer.\n"
               "Use it when an interactive session records archives; success requires the native output receipt.\n";
        return 0;
      }
      if (arg == "--force") {
        overwrite = true;
        continue;
      }
      if (arg == "--calibration-prompts") {
        calibration_prompts = true;
        continue;
      }
      if (arg == "--interactive") {
        interactive = true;
        continue;
      }
      if (arg == "--interactive-publication") {
        interactive = interactive_publication = true;
        continue;
      }
      if (arg == "--runtime-only") {
        interactive = runtime_only = true;
        continue;
      }
      if (i + 1 == argc)
        throw std::runtime_error("missing value for " + arg);
      const std::string value(argv[++i]);
      if (arg == "--game-dir")
        game = value;
      else if (arg == "--output")
        output = value;
      else if (arg == "--runner")
        runner = value;
      else if (arg == "--finalizer")
        finalizer = value;
      else if (arg == "--config")
        config = value;
      else if (arg == "--working-directory")
        working = value;
      else if (arg == "--enable-sinks")
        requested_sinks = value;
      else if (arg == "--sbatch") {
        if (value.rfind("--", 0) != 0 || value.find_first_of("\r\n") != std::string::npos)
          throw std::runtime_error("each --sbatch value must be one directive beginning with --");
        directives.push_back(value);
      } else
        throw std::runtime_error("unknown option: " + arg);
    }
    if (game.empty())
      throw std::runtime_error("--game-dir is required");
    if (runtime_only && interactive_publication)
      throw std::runtime_error("--runtime-only cannot publish interactive archives");
    // Preserve a symlinked game's logical name, which is also its CLI game ID.
    game = fs::absolute(game).lexically_normal();
    if (game.filename().empty())
      game = game.parent_path();
    if (game.filename().empty() || !fs::is_directory(game))
      throw std::runtime_error("--game-dir must name an existing game directory");
    const YAML::Node saved = YAML::LoadFile((game / "config.yaml").string());
    if (!saved.IsMap())
      throw std::runtime_error("game config.yaml must be a mapping");
    // Export can happen on a different host, without the execution runtime.
    // Only explicit overrides may bind the script to a config or directory.
    if (!working.empty())
      working = fs::absolute(working);
    if (!config.empty())
      config = fs::absolute(config);
    if (!config.empty() && config.extension() != ".yaml" && config.extension() != ".yml")
      throw std::runtime_error("encoding jobs require YAML pipeline configs (.yaml or .yml)");
    std::vector<std::string> args = {"-g", game.filename().string()};
    if (!config.empty()) {
      args.push_back("-c");
      args.push_back(config.string());
    }
    args.push_back("--enable-sources=URI-MULTIPLE");
    const YAML::Node ui = saved["hstream_ui"];
    const YAML::Node job = ui ? ui["job"] : YAML::Node();
    const YAML::Node saved_args = job && job.IsMap() ? job["arguments"] : YAML::Node();
    if (saved_args.IsDefined() && !saved_args.IsNull() && !saved_args.IsSequence())
      throw std::runtime_error("hstream_ui.job.arguments must be a sequence");
    std::vector<std::string> job_args;
    if (interactive) {
      if (!requested_sinks.empty())
        throw std::runtime_error("interactive sinks must be saved in hstream_ui.job.arguments");
      if (saved_args.IsSequence()) {
        for (const auto& argument : saved_args)
          job_args.push_back(argument.as<std::string>());
      } else {
        job_args.push_back("--enable-sinks=WEBRTC");
      }
    } else {
      job_args = encode_job_arguments(saved_args, requested_sinks);
    }
    if (interactive_publication)
      validate_interactive_publication(job_args);
    args.insert(args.end(), job_args.begin(), job_args.end());
    if (std::find(job_args.begin(), job_args.end(), "--stitching-calibration-only") != job_args.end()) {
      args.push_back("--options=hstream_ui.job.capture_telemetry=false");
    } else if (job && job.IsMap() && job["capture_telemetry"].IsDefined()) {
      args.push_back(
          std::string("--options=hstream_ui.job.capture_telemetry=") +
          (job["capture_telemetry"].as<bool>() ? "true" : "false"));
    }
    if (!saved_args.IsSequence()) {
      args.push_back("--options=pipeline.hmaudio.enable=1");
    }
    if (output.empty())
      output = game / "hstream-job.sh";
    output = fs::absolute(output);
    std::string script = "#!/usr/bin/env bash\n#SBATCH --nodes=1\n#SBATCH --ntasks=1\n#SBATCH --gres=gpu:1\n";
    for (const auto& directive : directives)
      script += "#SBATCH " + directive + "\n";
    script += "\n# Run directly, or submit this file with sbatch.\n";
    script += interactive && !interactive_publication
        ? "# hstream-cli must be on PATH on the execution host.\n"
        : "# hstream-finalize and hstream-cli must be on PATH on the execution host.\n";
    script += "# Game files and assets must be accessible there.\n";
    script += interactive && !interactive_publication
        ? "# Interactive session; the outer supervisor owns cleanup and cancellation.\n"
        : "# Publishes completed archives and telemetry, then cleans this run.\n"
          "# Repeating this script resumes the same run; export again for a new generation.\n";
    script += "set -euo pipefail\numask 077\n";
    if (!working.empty())
      script += "cd -- " + quote(working.string()) + "\n";
    script += "unset HSTREAM_UI_PARENT_PID\n";
    if (!interactive && !calibration_prompts)
      script += "unset HSTREAM_RINK_LEVELING_FLOW HSTREAM_PROJECTION_CROP_FLOW\n";
    if (!interactive)
      script +=
          "unset HSTREAM_CALIBRATION_PENDING HSTREAM_CALIBRATION_START_STAGE HSTREAM_CALIBRATION_INVALIDATION_ID\n";
    script +=
        "unset HSTREAM_ARCHIVE_RUN_ID HSTREAM_UI_PREVIEW_OVERLAYS HSTREAM_JOB_MANIFEST\n"
        "unset HSTREAM_JOB_PLUGIN_STAGE HSTREAM_JOB_LIBRARY_STAGE HSTREAM_JOB_MANAGED_RUNTIME\n";
    script += "export HM_GAME_DIR=" + quote(game.parent_path().string()) + "\n";
    for (const char* key : {"HM_OUTPUT_WORK_DIR", "HM_RENDER_SINK", "USE_NEW_NVSTREAMMUX"}) {
      const std::string value = environment(key);
      if (!value.empty())
        script += "export " + std::string(key) + "=" + quote(value) + "\n";
    }
    if (interactive && !interactive_publication) {
      // These sessions have no finalizer to admit them, so the supervisor owns
      // the private runtime directory. Refuse to share another run's cache.
      script += R"JOB(if [ -z "${HSTREAM_RUNTIME_CACHE_DIR:-}" ] || ! mkdir -p -- "${HSTREAM_RUNTIME_CACHE_DIR}"; then
  echo "Managed runtime directory is unavailable" >&2
  exit 78
fi
# A plain assignment, so an unsearchable directory aborts here. `export` would
# report its own success and hand the runner an empty shared-cache fallback.
runtime_cache="$(CDPATH= cd -- "${HSTREAM_RUNTIME_CACHE_DIR}" && pwd -P)"
# Check what the runner will actually use: command substitution strips trailing
# newlines, so a padded value resolves to a sibling that was never created.
if [ ! -w "${runtime_cache}" ]; then
  echo "Managed runtime directory is unavailable" >&2
  exit 78
fi
export HSTREAM_RUNTIME_CACHE_DIR="${runtime_cache}"
# The installed launcher rewrites ordinary arguments; managed runs own theirs.
export HSTREAM_JOB_MANAGED_RUNTIME=1
)JOB";
      script += "exec " + quote(runner);
      if (!runtime_only) {
        for (const auto& argument : args)
          script += " \\\n  " + quote(argument);
      }
      script += " \\\n  \"$@\"\n";
      write_script(output, script, overwrite);
      std::cout << output.string() << "\n";
      return 0;
    }
    script += "printf '%s\n' " +
        quote("hstream-job: Publishing completed outputs to " + game.string() +
              "; temporary files are removed only after all outputs are safely saved.") +
        "\n";
    if (interactive_publication)
      script += "export HSTREAM_JOB_MANAGED_RUNTIME=1\n";
    script += "exec " + quote(finalizer) + " --game-dir " + quote(game.string()) + " --run-id " +
        quote(hm::recording::NewGuid());
    // The supervisor resolves the same live config layers before it can admit
    // the runner. Forward option groups separately from the runner arguments.
    for (size_t i = 0; i < args.size(); ++i) {
      const auto& argument = args[i];
      if (argument == "--options" || argument == "-p") {
        if (++i == args.size())
          throw std::runtime_error("saved --options is missing its value");
        script += " --options " + quote(args[i]);
      } else if (argument.rfind("--options=", 0) == 0) {
        script += " --options " + quote(argument.substr(std::string("--options=").size()));
      } else if (argument.rfind("-p", 0) == 0 && argument.size() > 2) {
        script += " --options " + quote(argument.substr(2));
      }
    }
    script += " -- " + quote(runner);
    for (const auto& arg : args)
      script += " \\\n  " + quote(arg);
    if (interactive_publication)
      script += " \"$@\"";
    script += "\n";
    write_script(output, script, overwrite);
    std::cout << output.string() << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hstream-job: " << error.what() << "\n";
    return 1;
  }
}
