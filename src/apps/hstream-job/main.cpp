#include <yaml-cpp/yaml.h>
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
// encoder; an empty archive selection means the default 4K Program copy.
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
  if (encoders.empty())
    encoders.push_back("ENCODE_PROGRAM_4K_FILE");
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

fs::path executable() {
  return fs::read_symlink("/proc/self/exe");
}

fs::path bazel_bin_for_runner(const fs::path& runner) {
  if (runner.filename() != "hstream-cli")
    return {};
  const fs::path hstream_cli = runner.parent_path();
  const fs::path apps = hstream_cli.parent_path();
  const fs::path src = apps.parent_path();
  if (hstream_cli.filename() != "hstream-cli" || apps.filename() != "apps" || src.filename() != "src")
    return {};
  const fs::path bazel_bin = src.parent_path();
  return bazel_bin.filename() == "bin" ? bazel_bin : fs::path();
}

std::string runtime_environment_script(const fs::path& runtime_root, const fs::path& bazel_bin, bool installed) {
  std::string script = "HSTREAM_JOB_RUNTIME_ROOT=" + quote(runtime_root.string()) + "\n";
  script += "HSTREAM_JOB_BAZEL_BIN=" + quote(bazel_bin.string()) + "\n";
  script += R"JOB(
prepend_runtime_path() {
  local name="$1"
  local directory="$2"
  local current="${!name-}"
  if [ -z "${directory}" ] || [ ! -d "${directory}" ]; then
    return
  fi
  if [ -z "${current}" ]; then
    export "${name}=${directory}"
    return
  fi
  case ":${current}:" in
    *":${directory}:"*) ;;
    *) export "${name}=${directory}:${current}" ;;
  esac
}

if [ -z "${HSTREAM_RUNTIME_CACHE_DIR:-}" ] || \
    ! mkdir -p "${HSTREAM_RUNTIME_CACHE_DIR}" || [ ! -w "${HSTREAM_RUNTIME_CACHE_DIR}" ]; then
  echo "Managed runtime directory is unavailable" >&2
  exit 78
fi
runtime_cache="$(cd "${HSTREAM_RUNTIME_CACHE_DIR}" && pwd -P)"
export HSTREAM_RUNTIME_CACHE_DIR="${runtime_cache}"
export USE_NEW_NVSTREAMMUX="${USE_NEW_NVSTREAMMUX:-yes}"

runtime_arch="$(uname -m)"
output_configuration="installed"
if [ -n "${HSTREAM_JOB_BAZEL_BIN}" ]; then
  canonical_bazel_bin="$(readlink -f "${HSTREAM_JOB_BAZEL_BIN}")"
  output_configuration="$(basename "$(dirname "${canonical_bazel_bin}")")"
  output_configuration="$(printf '%s' "${output_configuration}" | tr -c 'A-Za-z0-9_.-' '_')"
fi
registry_dir="${runtime_cache}/gstreamer-1.0"
mkdir -p "${registry_dir}"
export GST_REGISTRY="${registry_dir}/registry.hstream.native-onnx-v1.${runtime_arch}.${output_configuration}.bin"

prepend_runtime_path GST_PLUGIN_PATH "${HSTREAM_JOB_RUNTIME_ROOT}/lib/gst-plugins"
prepend_runtime_path GST_PLUGIN_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_runtime_path LD_LIBRARY_PATH "${HSTREAM_JOB_RUNTIME_ROOT}/lib"
prepend_runtime_path LD_LIBRARY_PATH "${HSTREAM_JOB_RUNTIME_ROOT}/lib/gst-plugins"
prepend_runtime_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_runtime_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
)JOB";
  if (installed) {
    script += R"JOB(prepend_runtime_path PATH "${HSTREAM_JOB_RUNTIME_ROOT}/bin"
prepend_runtime_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/nvshmem/13"
prepend_runtime_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/libcusparseLt/13"
prepend_runtime_path LD_LIBRARY_PATH "/usr/lib/aarch64-linux-gnu/tegra"
prepend_runtime_path LD_LIBRARY_PATH "/usr/local/cuda/targets/aarch64-linux/lib"
)JOB";
  }
  script += R"JOB(
if [ -n "${HSTREAM_JOB_BAZEL_BIN}" ]; then
  bazel_gst_root="${HSTREAM_JOB_BAZEL_BIN}/src/gst-plugins"
  if [ -d "${bazel_gst_root}" ]; then
    mkdir -p "${runtime_cache}/gst-plugin-path/${runtime_arch}/${output_configuration}"
    runtime_plugin_dir="$(mktemp -d "${runtime_cache}/gst-plugin-path/${runtime_arch}/${output_configuration}/launch-XXXXXX")"
    export HSTREAM_JOB_PLUGIN_STAGE="${runtime_plugin_dir}"
    while IFS= read -r plugin_so; do
      plugin_so="$(readlink -f "${plugin_so}")"
      ln -sfn "${plugin_so}" "${runtime_plugin_dir}/$(basename "${plugin_so}")"
      prepend_runtime_path LD_LIBRARY_PATH "$(dirname "${plugin_so}")"
    done < <(
      find "${bazel_gst_root}" \
        -mindepth 2 \
        -maxdepth 3 \
        -type f \
        \( -name 'libnvdsgst_*.so' -o -name 'libgst*.so' \) \
        ! -path '*/testutils/*' \
        ! -path '*.runfiles/*' \
        -print | sort
    )
    while IFS= read -r library_dir; do
      prepend_runtime_path LD_LIBRARY_PATH "$(readlink -f "${library_dir}")"
    done < <(
      find "${bazel_gst_root}" \
        -mindepth 2 \
        -maxdepth 4 \
        -type f \
        -name '*.so' \
        ! -path '*.runfiles/*' \
        -printf '%h\n' | sort -u
    )
    prepend_runtime_path GST_PLUGIN_PATH "${runtime_plugin_dir}"
  fi

  case "${runtime_arch}" in
    x86_64 | amd64) solib_name="_solib_k8" ;;
    aarch64 | arm64) solib_name="_solib_aarch64" ;;
    *) solib_name="_solib_unknown" ;;
  esac
  solib_root="${HSTREAM_JOB_BAZEL_BIN}/${solib_name}"
  if [ -d "${solib_root}" ]; then
    while IFS= read -r solib_dir; do
      prepend_runtime_path LD_LIBRARY_PATH "$(readlink -f "${solib_dir}")"
    done < <(
      find "${solib_root}" \
        -maxdepth 1 \
        -type d \
        ! -path '*Sstubs*' \
        -print | sort
    )

    mkdir -p "${runtime_cache}/runtime-lib-path/${runtime_arch}/${output_configuration}"
    runtime_lib_dir="$(mktemp -d "${runtime_cache}/runtime-lib-path/${runtime_arch}/${output_configuration}/launch-XXXXXX")"
    export HSTREAM_JOB_LIBRARY_STAGE="${runtime_lib_dir}"
    onnxruntime_so="$({
      find -L "${solib_root}" -maxdepth 3 -type f -name 'libonnxruntime.so.1' -print -quit
    } 2>/dev/null || true)"
    if [ -n "${onnxruntime_so}" ]; then
      ln -sfn "$(readlink -f "${onnxruntime_so}")" "${runtime_lib_dir}/libonnxruntime.so.1"
      onnxruntime_source="$(dirname "$(readlink -f "${onnxruntime_so}")")"
      # ORT opens providers beside the loaded core library, including this cache.
      for onnxruntime_name in libonnxruntime_providers_shared.so libonnxruntime_providers_cuda.so; do
        if [ ! -f "${onnxruntime_source}/${onnxruntime_name}" ]; then
          echo "Missing ONNX Runtime library: ${onnxruntime_source}/${onnxruntime_name}" >&2
          exit 78
        fi
        ln -sfn "$(readlink -f "${onnxruntime_source}/${onnxruntime_name}")" "${runtime_lib_dir}/${onnxruntime_name}"
      done
    fi
    yolo_so="${HSTREAM_JOB_BAZEL_BIN}/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so"
    if [ -e "${yolo_so}" ]; then
      ln -sfn "$(readlink -f "${yolo_so}")" "${runtime_lib_dir}/libnvdsinfer_custom_impl_Yolo.so"
    fi
    prepend_runtime_path LD_LIBRARY_PATH "${runtime_lib_dir}"
  fi
fi

unset HSTREAM_JOB_RUNTIME_ROOT HSTREAM_JOB_BAZEL_BIN
)JOB";
  return script;
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
    fs::path game, output, runner, config, working, finalizer;
    std::string requested_sinks;
    std::vector<std::string> directives;
    bool overwrite = false, interactive = false, runtime_only = false, calibration_prompts = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg(argv[i]);
      if (arg == "--help" || arg == "-h") {
        std::cout << "Usage: hstream-job --game-dir DIR [--output FILE] [--force] [--interactive]\n"
                     "  [--sbatch '--partition=gpu'] [--sbatch '--time=02:00:00']\n"
                     "  [--runner FILE] [--finalizer FILE] [--config FILE] [--working-directory DIR]\n"
                     "  [--enable-sinks ENCODE_FILE,ENCODE_STITCHED_FILE,ENCODE_PROGRAM_4K_FILE]\n"
                     "Save an executable job (default: DIR/hstream-job.sh). Never launches the job.\n"
                     "SBATCH defaults: one node, one task, one GPU. Additional directives override defaults.\n"
                     "Reads config.yaml and hstream_ui.job.arguments saved by the UI. Without a saved\n"
                     "job or selected file encoder, defaults to the 4K Program MP4. Rendering is disabled.\n"
                     "DriveGPT capture defaults on; the UI's explicit checkbox choice is preserved.\n"
                     "Successful jobs publish faststart MP4s/databases to the game and clean their working files.\n"
                     "--interactive preserves the saved sinks and stdin, uses the same native runtime setup,\n"
                     "and runs without archive publication. Its supervisor must set a private\n"
                     "HSTREAM_RUNTIME_CACHE_DIR and clean it after every descendant has stopped.\n"
                     "Extra script arguments are appended as literal runner arguments in interactive mode.\n";
        std::cout << "--calibration-prompts preserves explicitly configured rink-leveling and crop flows\n"
                     "for managed jobs; the supervisor must service calibration prompts.\n";
        std::cout << "--runtime-only uses the same setup but forwards only the script's arguments, for native\n"
                     "sampling and runtime-info subcommands that cannot inherit a saved playback recipe.\n";
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
    // Preserve a symlinked game's logical name, which is also its CLI game ID.
    game = fs::absolute(game).lexically_normal();
    if (game.filename().empty())
      game = game.parent_path();
    if (game.filename().empty() || !fs::is_directory(game))
      throw std::runtime_error("--game-dir must name an existing game directory");
    const YAML::Node saved = YAML::LoadFile((game / "config.yaml").string());
    if (!saved.IsMap())
      throw std::runtime_error("game config.yaml must be a mapping");
    const fs::path bin = executable().parent_path();
    const bool installed = fs::exists(bin / "hstream-cli");
    if (finalizer.empty())
      finalizer = installed ? bin / "hstream-finalize" : bin.parent_path() / "hstream-finalize/hstream-finalize";
    if (!interactive && !fs::is_regular_file(finalizer))
      throw std::runtime_error("hstream-finalize not found; build it or specify --finalizer");
    if (!interactive)
      finalizer = fs::canonical(finalizer);
    if (runner.empty())
      runner = installed ? bin / "hstream-cli" : bin.parent_path() / "hstream-cli/hstream-cli";
    if (working.empty()) {
      if (installed)
        working = bin.parent_path();
      else if (!environment("BUILD_WORKSPACE_DIRECTORY").empty())
        working = environment("BUILD_WORKSPACE_DIRECTORY");
      else {
        // Bazel runfiles expose source configs without relying on the caller's cwd.
        const fs::path runfiles = executable().string() + ".runfiles";
        for (const char* repo : {"kstream", "_main"}) {
          if (fs::exists(runfiles / repo / "configs/ds_hockey_app_config.yaml")) {
            working = fs::canonical(runfiles / repo / "configs/ds_hockey_app_config.yaml").parent_path().parent_path();
            break;
          }
        }
        if (working.empty())
          working = fs::current_path();
      }
    }
    working = fs::absolute(working);
    if (config.empty())
      config = working / "configs/ds_hockey_app_config.yaml";
    config = fs::absolute(config);
    if (!fs::is_regular_file(config))
      throw std::runtime_error("pipeline config not found; specify --config: " + config.string());
    if (config.extension() != ".yaml" && config.extension() != ".yml")
      throw std::runtime_error("encoding jobs require YAML pipeline configs (.yaml or .yml)");
    if (!fs::is_regular_file(runner))
      throw std::runtime_error("hstream-cli not found; build it or specify --runner: " + runner.string());
    runner = fs::canonical(runner);
    const fs::path runtime_root = installed ? bin.parent_path() : working;
    const fs::path bazel_bin = bazel_bin_for_runner(runner);
    std::vector<std::string> args = {
        "-g", game.filename().string(), "-c", config.string(), "--enable-sources=URI-MULTIPLE"};
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
    script +=
        "\n# Run directly, or submit this file with sbatch.\n"
        "# Configs, videos and assets must be accessible on the execution host.\n";
    script += interactive ? "# Interactive session; the outer supervisor owns cleanup and cancellation.\n"
                          : "# Publishes completed archives and telemetry, then cleans this run.\n"
                            "# Repeating this script resumes the same run; export again for a new generation.\n";
    script += "set -euo pipefail\numask 077\n";
    script += "cd -- " + quote(working.string()) + "\n";
    script += "unset HSTREAM_UI_PARENT_PID\n";
    if (!interactive && !calibration_prompts)
      script += "unset HSTREAM_RINK_LEVELING_FLOW HSTREAM_PROJECTION_CROP_FLOW\n";
    if (!interactive)
      script +=
          "unset HSTREAM_CALIBRATION_PENDING HSTREAM_CALIBRATION_START_STAGE HSTREAM_CALIBRATION_INVALIDATION_ID\n";
    script +=
        "unset HSTREAM_ARCHIVE_RUN_ID HSTREAM_UI_PREVIEW_OVERLAYS HSTREAM_JOB_MANIFEST\n"
        "unset HSTREAM_JOB_PLUGIN_STAGE HSTREAM_JOB_LIBRARY_STAGE\n";
    script += "export HM_GAME_DIR=" + quote(game.parent_path().string()) + "\n";
    for (const char* key : {"HM_CONFIG_ROOT", "HM_OUTPUT_WORK_DIR", "HM_RENDER_SINK", "USE_NEW_NVSTREAMMUX"}) {
      const std::string value = environment(key);
      if (!value.empty())
        script += "export " + std::string(key) + "=" + quote(value) + "\n";
    }
    // Stage only after the supervisor admits this run and assigns its private
    // runtime directory. Busy or completed retries must not create scratch.
    const std::string child_setup =
        "set -euo pipefail\n" + runtime_environment_script(runtime_root, bazel_bin, installed) + "\nexec \"$@\"\n";
    if (interactive) {
      script += "exec /bin/bash -c " + quote(child_setup) + " hstream-runtime " + quote(runner.string());
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
    script += "exec " + quote(finalizer.string()) + " --game-dir " + quote(game.string()) + " --run-id " +
        quote(hm::recording::NewGuid());
    // The supervisor resolves the same live config layers before it can admit
    // the runner. Forward option groups separately from the shell wrapper.
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
    script += " -- /bin/bash -c " + quote(child_setup) + " hstream-runtime " + quote(runner.string());
    for (const auto& arg : args)
      script += " \\\n  " + quote(arg);
    script += "\n";
    write_script(output, script, overwrite);
    std::cout << output.string() << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hstream-job: " << error.what() << "\n";
    return 1;
  }
}
