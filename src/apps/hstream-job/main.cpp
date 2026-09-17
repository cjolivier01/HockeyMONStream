#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {
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

runtime_cache=""
for candidate in \
  "${HSTREAM_RUNTIME_CACHE_DIR:-}" \
  "${HSTREAM_JOB_RUNTIME_ROOT}/.cache" \
  "${TEST_TMPDIR:+${TEST_TMPDIR}/hstream-runtime-cache}" \
  "${XDG_CACHE_HOME:+${XDG_CACHE_HOME}/hstream}" \
  "${HOME:+${HOME}/.cache/hstream}"; do
  if [ -n "${candidate}" ] && mkdir -p "${candidate}" 2>/dev/null && [ -w "${candidate}" ]; then
    runtime_cache="$(cd "${candidate}" && pwd -P)"
    break
  fi
done
if [ -z "${runtime_cache}" ]; then
  runtime_cache="$(mktemp -d "${TMPDIR:-/tmp}/hstream-runtime.XXXXXX")"
fi
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
  launch_key="launch-${BASHPID:-$$}"
  bazel_gst_root="${HSTREAM_JOB_BAZEL_BIN}/src/gst-plugins"
  if [ -d "${bazel_gst_root}" ]; then
    runtime_plugin_dir="${runtime_cache}/gst-plugin-path/${runtime_arch}/${output_configuration}/${launch_key}"
    mkdir -p "${runtime_plugin_dir}"
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

    runtime_lib_dir="${runtime_cache}/runtime-lib-path/${runtime_arch}/${output_configuration}/${launch_key}"
    mkdir -p "${runtime_lib_dir}"
    onnxruntime_so="$({
      find -L "${solib_root}" -maxdepth 3 -type f -name 'libonnxruntime.so.1' -print -quit
    } 2>/dev/null || true)"
    if [ -n "${onnxruntime_so}" ]; then
      ln -sfn "$(readlink -f "${onnxruntime_so}")" "${runtime_lib_dir}/libonnxruntime.so.1"
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
    fs::path game, output, runner, config, working;
    std::vector<std::string> directives;
    bool overwrite = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg(argv[i]);
      if (arg == "--help" || arg == "-h") {
        std::cout << "Usage: hstream-job --game-dir DIR [--output FILE] [--force]\n"
                     "  [--sbatch '--partition=gpu'] [--sbatch '--time=02:00:00']\n"
                     "  [--runner FILE] [--config FILE] [--working-directory DIR]\n"
                     "Save an executable job (default: DIR/hstream-job.sh). Never launches the job.\n"
                     "SBATCH defaults: one node, one task, one GPU. Additional directives override defaults.\n"
                     "Reads config.yaml and hstream_ui.job.arguments saved by the UI. Without a saved\n"
                     "job, uses the configured pipeline sinks. Scripts reference existing configs and assets.\n"
                     "Archives/databases remain in CLI working storage; no UI MP4 remux or game-directory copy.\n";
        return 0;
      }
      if (arg == "--force") {
        overwrite = true;
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
      else if (arg == "--config")
        config = value;
      else if (arg == "--working-directory")
        working = value;
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
    if (saved_args && saved_args.IsSequence()) {
      for (const auto& value : saved_args)
        args.push_back(value.as<std::string>());
    } else {
      // Let the CLI interpret the canonical game configuration and its sink enable flags.
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
        "# Configs, videos and assets must be accessible on the execution host.\n"
        "# Rendering requires a display; disable Render video for a headless job.\n"
        "# Archive MKVs and DriveGPT databases stay in CLI working storage.\n"
        "# Play-only MP4 remux and game-directory publication are not performed.\n"
        "set -euo pipefail\n";
    script += "cd -- " + quote(working.string()) + "\n";
    script +=
        "unset HSTREAM_UI_PARENT_PID HSTREAM_RINK_LEVELING_FLOW HSTREAM_PROJECTION_CROP_FLOW\n"
        "unset HSTREAM_CALIBRATION_PENDING HSTREAM_CALIBRATION_START_STAGE HSTREAM_CALIBRATION_INVALIDATION_ID\n"
        "unset HSTREAM_ARCHIVE_RUN_ID HSTREAM_UI_PREVIEW_OVERLAYS\n";
    script += "export HM_GAME_DIR=" + quote(game.parent_path().string()) + "\n";
    for (const char* key : {"HM_CONFIG_ROOT", "HM_OUTPUT_WORK_DIR", "HM_RENDER_SINK", "USE_NEW_NVSTREAMMUX"}) {
      const std::string value = environment(key);
      if (!value.empty())
        script += "export " + std::string(key) + "=" + quote(value) + "\n";
    }
    script += runtime_environment_script(runtime_root, bazel_bin, installed);
    const std::string configured_output = environment("HM_OUTPUT_WORK_DIR");
    const std::string output_location =
        configured_output.empty() ? "the configured output root (normally ~/hstream_output)" : configured_output;
    script += "printf '%s\\n' " +
        quote("hstream-job: Archive MKVs and DriveGPT databases remain in CLI working storage under " +
              output_location +
              "; custom output paths still apply. See CLI logs for resolved files. "
              "Play-only MP4 remux and copying into the game directory are not performed.") +
        "\n";
    script += "exec " + quote(runner.string());
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
