#include <NvInfer.h>
#include <NvInferVersion.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#include <opencv2/opencv.hpp>
#include "EnginePrecision.h"

#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#if NV_TENSORRT_MAJOR < 11
#define HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR 1
#else
#define HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR 0
#endif

class Logger : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* msg) noexcept override {
    if (severity <= Severity::kINFO) {
      std::cerr << "[TRT] " << msg << "\n";
    }
  }
};

struct Args {
  std::string onnx;
  std::string image_list;
  std::string calib_table;
  std::string engine;
  std::string input_name;
  std::string precision = "int8";
  int batch_size = 2;
  int min_batch_size = 1;
  int workspace_mb = 2048;
  float scale = 1.0f / 255.0f;
  bool rgb = true;
  bool fp16 = true;
  bool explicit_precision = false;
};

std::string require_value(int& i, int argc, char** argv) {
  if (i + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") + argv[i]);
  }
  return argv[++i];
}

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto value_after_equals = [&]() -> std::string {
      auto pos = arg.find('=');
      if (pos == std::string::npos) {
        return require_value(i, argc, argv);
      }
      return arg.substr(pos + 1);
    };

    if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: int8-calib-builder --precision int8|bf16 --onnx FILE --engine FILE [options]\n"
          << "Options:\n"
          << "  --precision P        Engine precision to build: int8 or bf16. Default: int8\n"
          << "                       INT8 calibration requires TensorRT's legacy calibrator API.\n"
          << "  --explicit-precision Use an already typed BF16 or calibrated INT8 Q/DQ ONNX (required on TRT 11+)\n"
          << "  --image-list FILE    Required for int8 calibration\n"
          << "  --calib-table FILE   Required for int8 calibration\n"
          << "  --batch-size N       Calibration/build batch size. Default: 2\n"
          << "  --min-batch-size N   Minimum runtime batch size in the TensorRT profile. Default: 1\n"
          << "  --input-name NAME    Override ONNX input tensor name\n"
          << "  --workspace-mb N     TensorRT workspace size in MiB. Default: 2048\n"
          << "  --scale F            Input scale factor. Default: 1/255\n"
          << "  --bgr                Keep OpenCV BGR channel order instead of RGB\n"
          << "  --no-fp16            Do not allow FP16 tactics while building INT8 engine\n";
      std::exit(0);
    } else if (arg == "--onnx" || arg.rfind("--onnx=", 0) == 0) {
      args.onnx = value_after_equals();
    } else if (arg == "--precision" || arg.rfind("--precision=", 0) == 0) {
      args.precision = value_after_equals();
    } else if (arg == "--image-list" || arg.rfind("--image-list=", 0) == 0) {
      args.image_list = value_after_equals();
    } else if (arg == "--calib-table" || arg.rfind("--calib-table=", 0) == 0) {
      args.calib_table = value_after_equals();
    } else if (arg == "--engine" || arg.rfind("--engine=", 0) == 0) {
      args.engine = value_after_equals();
    } else if (arg == "--input-name" || arg.rfind("--input-name=", 0) == 0) {
      args.input_name = value_after_equals();
    } else if (arg == "--batch-size" || arg.rfind("--batch-size=", 0) == 0) {
      args.batch_size = std::stoi(value_after_equals());
    } else if (arg == "--min-batch-size" || arg.rfind("--min-batch-size=", 0) == 0) {
      args.min_batch_size = std::stoi(value_after_equals());
    } else if (arg == "--workspace-mb" || arg.rfind("--workspace-mb=", 0) == 0) {
      args.workspace_mb = std::stoi(value_after_equals());
    } else if (arg == "--scale" || arg.rfind("--scale=", 0) == 0) {
      args.scale = std::stof(value_after_equals());
    } else if (arg == "--bgr") {
      args.rgb = false;
    } else if (arg == "--explicit-precision") {
      args.explicit_precision = true;
    } else if (arg == "--no-fp16") {
      args.fp16 = false;
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (args.precision != "int8" && args.precision != "bf16") {
    throw std::runtime_error("--precision must be int8 or bf16");
  }
  if (args.onnx.empty() || args.engine.empty()) {
    throw std::runtime_error("--onnx and --engine are required");
  }
  if (args.precision == "int8" && !args.explicit_precision && (args.image_list.empty() || args.calib_table.empty())) {
    throw std::runtime_error("--image-list and --calib-table are required for --precision=int8");
  }
  if (args.explicit_precision && (!args.image_list.empty() || !args.calib_table.empty())) {
    throw std::runtime_error("--explicit-precision consumes a prepared ONNX; do not supply legacy calibration inputs");
  }
  if (args.workspace_mb <= 0) {
    throw std::runtime_error("--workspace-mb must be positive");
  }
#if !HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR
  if (!args.explicit_precision) {
    throw std::runtime_error(
        "TensorRT 11+ requires --explicit-precision with a typed BF16 or calibrated INT8 Q/DQ ONNX. "
        "For DeepStream using TensorRT 10, build this tool with its matching TensorRT SDK.");
  }
#endif
  if (args.batch_size <= 0) {
    throw std::runtime_error("--batch-size must be positive");
  }
  if (args.min_batch_size <= 0 || args.min_batch_size > args.batch_size) {
    throw std::runtime_error("--min-batch-size must be positive and no larger than --batch-size");
  }
  return args;
}

#if HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR
void check_cuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
  }
}

std::vector<std::string> read_lines(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open image list: " + path);
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  if (lines.empty()) {
    throw std::runtime_error("image list is empty: " + path);
  }
  return lines;
}

std::vector<float> prepare_image(const cv::Mat& src_bgr, int channels, int height, int width, bool rgb, float scale) {
  if (channels != 3) {
    throw std::runtime_error("only 3-channel NCHW input is supported");
  }

  cv::Mat image;
  if (rgb) {
    cv::cvtColor(src_bgr, image, cv::COLOR_BGR2RGB);
  } else {
    image = src_bgr;
  }

  const float resize_scale = std::min(width / static_cast<float>(image.cols), height / static_cast<float>(image.rows));
  const int resized_w = std::max(1, static_cast<int>(std::round(image.cols * resize_scale)));
  const int resized_h = std::max(1, static_cast<int>(std::round(image.rows * resize_scale)));

  cv::Mat resized;
  cv::resize(image, resized, cv::Size(resized_w, resized_h), 0, 0, cv::INTER_LINEAR);

  cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(0, 0, 0));
  const int x = (width - resized_w) / 2;
  const int y = (height - resized_h) / 2;
  resized.copyTo(canvas(cv::Rect(x, y, resized_w, resized_h)));

  cv::Mat fp32;
  canvas.convertTo(fp32, CV_32F, scale);

  std::vector<cv::Mat> split_channels(channels);
  cv::split(fp32, split_channels);

  std::vector<float> chw(static_cast<size_t>(channels) * height * width);
  float* out = chw.data();
  const size_t channel_size = static_cast<size_t>(height) * width;
  for (int c = 0; c < channels; ++c) {
    std::memcpy(out, split_channels[c].data, channel_size * sizeof(float));
    out += channel_size;
  }
  return chw;
}

class ImageEntropyCalibrator : public nvinfer1::IInt8EntropyCalibrator2 {
 public:
  ImageEntropyCalibrator(
      int batch_size,
      int channels,
      int height,
      int width,
      std::string input_name,
      std::vector<std::string> image_paths,
      std::string cache_path,
      bool rgb,
      float scale)
      : batch_size_(batch_size),
        channels_(channels),
        height_(height),
        width_(width),
        input_name_(std::move(input_name)),
        image_paths_(std::move(image_paths)),
        cache_path_(std::move(cache_path)),
        rgb_(rgb),
        scale_(scale) {
    input_count_ = static_cast<size_t>(batch_size_) * channels_ * height_ * width_;
    host_batch_.resize(input_count_);
    check_cuda(cudaMalloc(&device_input_, input_count_ * sizeof(float)), "cudaMalloc calibration input");
  }

  ~ImageEntropyCalibrator() override {
    if (device_input_) {
      cudaFree(device_input_);
    }
  }

  int getBatchSize() const noexcept override {
    return batch_size_;
  }

  bool getBatch(void* bindings[], const char* names[], int nb_bindings) noexcept override {
    try {
      if (image_index_ + static_cast<size_t>(batch_size_) > image_paths_.size()) {
        return false;
      }

      float* dst = host_batch_.data();
      const size_t image_size = static_cast<size_t>(channels_) * height_ * width_;
      for (int b = 0; b < batch_size_; ++b) {
        const std::string& image_path = image_paths_[image_index_++];
        cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
        if (image.empty()) {
          failed_ = true;
          error_ = "failed to read calibration image: " + image_path;
          std::cerr << error_ << "\n";
          return false;
        }
        std::vector<float> chw = prepare_image(image, channels_, height_, width_, rgb_, scale_);
        std::memcpy(dst, chw.data(), image_size * sizeof(float));
        dst += image_size;
        std::cout << "Calibrating image " << image_index_ << "/" << image_paths_.size() << ": " << image_path << "\n";
      }

      check_cuda(
          cudaMemcpy(device_input_, host_batch_.data(), input_count_ * sizeof(float), cudaMemcpyHostToDevice),
          "cudaMemcpy calibration input");

      for (int i = 0; i < nb_bindings; ++i) {
        if (names[i] && input_name_ == names[i]) {
          bindings[i] = device_input_;
          return true;
        }
      }
      std::cerr << "failed to find calibration binding: " << input_name_ << "\n";
      failed_ = true;
      error_ = "failed to find calibration binding: " + input_name_;
      return false;
    } catch (const std::exception& exc) {
      failed_ = true;
      error_ = std::string("calibration batch failed: ") + exc.what();
      std::cerr << error_ << "\n";
      return false;
    }
  }

  const void* readCalibrationCache(std::size_t& length) noexcept override {
    cache_.clear();
    length = 0;
    return nullptr;
  }

  void writeCalibrationCache(const void* cache, std::size_t length) noexcept override {
    std::ofstream output(cache_path_, std::ios::binary);
    output.write(reinterpret_cast<const char*>(cache), static_cast<std::streamsize>(length));
    std::cout << "Wrote INT8 calibration cache: " << cache_path_ << " (" << length << " bytes)\n";
  }

  void verifyComplete() const {
    if (failed_) {
      throw std::runtime_error(error_);
    }
    if (image_index_ != image_paths_.size()) {
      throw std::runtime_error(
          "calibration consumed " + std::to_string(image_index_) + "/" + std::to_string(image_paths_.size()) +
          " images; image count must be divisible by batch size and all images must be readable");
    }
  }

 private:
  int batch_size_;
  int channels_;
  int height_;
  int width_;
  std::string input_name_;
  std::vector<std::string> image_paths_;
  std::string cache_path_;
  bool rgb_;
  float scale_;
  size_t image_index_ = 0;
  size_t input_count_ = 0;
  std::vector<float> host_batch_;
  void* device_input_ = nullptr;
  std::vector<char> cache_;
  bool failed_ = false;
  std::string error_;
};

#endif

void write_file(const std::string& path, const void* data, size_t size) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  if (!output) {
    throw std::runtime_error("failed to write output file: " + path);
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    const int runtime_version = getInferLibVersion();
    if (runtime_version / 10000 != NV_TENSORRT_MAJOR || (runtime_version / 100) % 100 != NV_TENSORRT_MINOR)
      throw std::runtime_error(
          "TensorRT SDK headers and loaded runtime differ; select a matching HSTREAM_TENSORRT_SDK_ROOT");
    std::cout << "TensorRT runtime version: " << getInferLibVersion() << " (SDK " << NV_TENSORRT_MAJOR << '.'
              << NV_TENSORRT_MINOR << ")\n";
#if HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR
    std::vector<std::string> image_paths;
    if (args.precision == "int8" && !args.explicit_precision)
      image_paths = read_lines(args.image_list);
#endif

    Logger logger;
    std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));
    if (!builder) {
      throw std::runtime_error("failed to create TensorRT builder");
    }

    nvinfer1::NetworkDefinitionCreationFlags flags = 0;
#if NV_TENSORRT_MAJOR < 10
    flags |= 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
#elif NV_TENSORRT_MAJOR < 11
    if (args.explicit_precision)
      flags |= 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kSTRONGLY_TYPED);
#endif
    std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(flags));
    if (!network) {
      throw std::runtime_error("failed to create TensorRT network");
    }

    std::unique_ptr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, logger));
    if (!parser || !parser->parseFromFile(args.onnx.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
      throw std::runtime_error("failed to parse ONNX: " + args.onnx);
    }
    if (network->getNbInputs() != 1) {
      throw std::runtime_error("expected exactly one network input");
    }

    nvinfer1::ITensor* input = network->getInput(0);
    const char* trt_input_name = input->getName();
    std::string input_name = args.input_name.empty() ? trt_input_name : args.input_name;
    nvinfer1::Dims input_dims = input->getDimensions();
    if (input_name != trt_input_name)
      throw std::runtime_error("--input-name must match the ONNX input tensor name");
    if (input_dims.nbDims != 4 || input_dims.d[1] != 3 || input_dims.d[2] <= 0 || input_dims.d[3] <= 0)
      throw std::runtime_error("expected NCHW input with three channels and fixed spatial dimensions");
    if (input_dims.d[0] != -1 && (input_dims.d[0] != args.batch_size || args.min_batch_size != args.batch_size))
      throw std::runtime_error("static ONNX batch must equal both --batch-size and --min-batch-size");
    if (input->getType() != nvinfer1::DataType::kFLOAT)
      throw std::runtime_error("detector input must remain FP32");
    for (int i = 0; i < network->getNbOutputs(); ++i) {
      if (network->getOutput(i)->getType() != nvinfer1::DataType::kFLOAT)
        throw std::runtime_error("detector output must remain FP32 for NvDsInferParseYolo");
    }
    if (args.explicit_precision) {
      bool quantize = false, dequantize = false, bf16 = false;
      for (int i = 0; i < network->getNbLayers(); ++i) {
        auto* layer = network->getLayer(i);
        quantize |= layer->getType() == nvinfer1::LayerType::kQUANTIZE &&
            layer->getOutput(0)->getType() == nvinfer1::DataType::kINT8;
        dequantize |= layer->getType() == nvinfer1::LayerType::kDEQUANTIZE;
#if NV_TENSORRT_MAJOR >= 9
        for (int j = 0; j < layer->getNbOutputs(); ++j)
          bf16 |= layer->getOutput(j)->getType() == nvinfer1::DataType::kBF16;
#endif
      }
      if (args.precision == "int8" ? !(quantize && dequantize) : !bf16)
        throw std::runtime_error("ONNX does not contain the requested explicit precision; prepare the model first");
    }
    const int channels = input_dims.d[1];
    const int height = input_dims.d[2];
    const int width = input_dims.d[3];

    std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
    if (!config) {
      throw std::runtime_error("failed to create TensorRT builder config");
    }
    config->setMemoryPoolLimit(
        nvinfer1::MemoryPoolType::kWORKSPACE, static_cast<size_t>(args.workspace_mb) * 1024 * 1024);
    config->setProfilingVerbosity(nvinfer1::ProfilingVerbosity::kDETAILED);
#if HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR
    if (!args.explicit_precision) {
      if (args.precision == "int8") {
        config->setFlag(nvinfer1::BuilderFlag::kINT8);
        if (args.fp16 && builder->platformHasFastFp16())
          config->setFlag(nvinfer1::BuilderFlag::kFP16);
      } else {
#if NV_TENSORRT_MAJOR >= 9
        config->setFlag(nvinfer1::BuilderFlag::kBF16);
#else
        throw std::runtime_error("BF16 requires TensorRT 9 or newer");
#endif
      }
    }
#endif

    nvinfer1::IOptimizationProfile* profile = nullptr;
    if (input_dims.d[0] < 0) {
      profile = builder->createOptimizationProfile();
      nvinfer1::Dims dims = input_dims;
      dims.d[0] = args.min_batch_size;
      if (!profile || !profile->setDimensions(trt_input_name, nvinfer1::OptProfileSelector::kMIN, dims))
        throw std::runtime_error("invalid minimum batch profile");
      dims.d[0] = args.batch_size;
      if (!profile->setDimensions(trt_input_name, nvinfer1::OptProfileSelector::kOPT, dims) ||
          !profile->setDimensions(trt_input_name, nvinfer1::OptProfileSelector::kMAX, dims) ||
          config->addOptimizationProfile(profile) < 0)
        throw std::runtime_error("invalid runtime batch profile");
#if HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR
      if (args.precision == "int8" && !args.explicit_precision) {
        config->setCalibrationProfile(profile);
      }
#endif
    }

#if HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR
    std::unique_ptr<ImageEntropyCalibrator> calibrator;
    if (args.precision == "int8" && !args.explicit_precision) {
      calibrator = std::make_unique<ImageEntropyCalibrator>(
          args.batch_size,
          channels,
          height,
          width,
          input_name,
          std::move(image_paths),
          args.calib_table,
          args.rgb,
          args.scale);
      config->setInt8Calibrator(calibrator.get());
    }
#endif

    std::cout << "Building " << args.precision << " engine from " << args.onnx << "\n"
              << "  input: " << input_name << " batch=" << args.batch_size << " chw=" << channels << "x" << height
              << "x" << width << "\n";
    if (args.precision == "int8" && !args.explicit_precision) {
      std::cout << "  calibration cache: " << args.calib_table << "\n";
    }
    std::cout << "  engine: " << args.engine << "\n";

    std::unique_ptr<nvinfer1::IHostMemory> serialized(builder->buildSerializedNetwork(*network, *config));
    if (!serialized) {
      throw std::runtime_error(
          "TensorRT failed to build serialized " + args.precision + " network" +
          (args.precision == "int8" && !args.explicit_precision
               ? "; use a calibrated Q/DQ ONNX with --explicit-precision when legacy calibration is unsupported"
               : ""));
    }
#if HSTREAM_HAS_TRT_LEGACY_INT8_CALIBRATOR
    if (calibrator) {
      calibrator->verifyComplete();
    }
#endif

    // Deserialize and inspect before publishing, keeping an existing engine
    // intact if build/validation fails. Detailed layer types aid accuracy audits.
    std::unique_ptr<nvinfer1::IRuntime> runtime(nvinfer1::createInferRuntime(logger));
    std::unique_ptr<nvinfer1::ICudaEngine> engine(
        runtime ? runtime->deserializeCudaEngine(serialized->data(), serialized->size()) : nullptr);
    if (!engine)
      throw std::runtime_error("could not deserialize the generated engine");
    std::unique_ptr<nvinfer1::IEngineInspector> inspector(engine->createEngineInspector());
    const char* details =
        inspector ? inspector->getEngineInformation(nvinfer1::LayerInformationFormat::kJSON) : nullptr;
    if (!details)
      throw std::runtime_error("could not inspect the generated engine");
    if (!hm::inference::EngineUsesPrecision(YAML::Load(details), args.precision))
      throw std::runtime_error(
          "engine inspection found no " + args.precision +
          " tensors; refusing to publish an engine that fell back to another precision");
    const std::string temporary = args.engine + ".tmp." + std::to_string(::getpid());
    write_file(temporary, serialized->data(), serialized->size());
    write_file(temporary + ".layers.json", details, std::strlen(details));
    std::filesystem::rename(temporary + ".layers.json", args.engine + ".layers.json");
    std::filesystem::rename(temporary, args.engine);
    std::cout << "Wrote " << args.precision << " engine: " << args.engine << " (" << serialized->size() << " bytes)\n";
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "int8-calib-builder: " << exc.what() << "\n";
    return 2;
  }
}
