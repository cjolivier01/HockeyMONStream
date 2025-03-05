#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// FFmpeg headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// Loads audio from a file (or video file) using FFmpeg and returns a mono audio signal
// as a vector of doubles along with its sample rate.
std::pair<std::vector<double>, int> load_audio_as_tensor(
    const std::string& file,
    double duration_seconds,
    bool verbose = false) {
  // In FFmpeg 4.0 and later, av_register_all() is deprecated and not needed.
  AVFormatContext* fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, file.c_str(), nullptr, nullptr) < 0) {
    std::cerr << "Could not open file: " << file << std::endl;
    std::exit(1);
  }
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    std::cerr << "Could not retrieve stream info from: " << file << std::endl;
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }

  // Find the first audio stream.
  int audio_stream_index = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_stream_index = i;
      break;
    }
  }
  if (audio_stream_index < 0) {
    std::cerr << "No audio stream found in: " << file << std::endl;
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }

  // Set up the codec context.
  AVCodecParameters* codecpar = fmt_ctx->streams[audio_stream_index]->codecpar;
  const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
  if (!codec) {
    std::cerr << "Codec not found." << std::endl;
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }
  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    std::cerr << "Could not allocate codec context." << std::endl;
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }
  if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
    std::cerr << "Could not copy codec parameters." << std::endl;
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }
  if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
    std::cerr << "Could not open codec." << std::endl;
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }

  // If the channel layout is not set, use the default.
  if (codec_ctx->channel_layout == 0) {
    codec_ctx->channel_layout = av_get_default_channel_layout(codec_ctx->channels);
  }

  // Set up the resampler to convert the audio to mono, float format.
  SwrContext* swr_ctx = swr_alloc();
  if (!swr_ctx) {
    std::cerr << "Could not allocate resampler context." << std::endl;
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }
  // Input settings.
  av_opt_set_int(swr_ctx, "in_channel_layout", codec_ctx->channel_layout, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);
  // Output settings: mono, same sample rate, float format.
  int64_t out_channel_layout = AV_CH_LAYOUT_MONO;
  av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
  av_opt_set_int(swr_ctx, "out_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
  if (swr_init(swr_ctx) < 0) {
    std::cerr << "Could not initialize resampler." << std::endl;
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }

  int out_sample_rate = codec_ctx->sample_rate;
  // Total number of output samples we want to read.
  int64_t total_output_samples = static_cast<int64_t>(duration_seconds * out_sample_rate);
  std::vector<double> audio_samples;
  audio_samples.reserve(total_output_samples);

  // Allocate packet and frame for decoding.
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool finished = false;

  while (!finished && av_read_frame(fmt_ctx, packet) >= 0) {
    if (packet->stream_index == audio_stream_index) {
      int ret = avcodec_send_packet(codec_ctx, packet);
      if (ret < 0) {
        std::cerr << "Error sending packet for decoding." << std::endl;
        break;
      }
      while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        if (ret < 0) {
          std::cerr << "Error during decoding." << std::endl;
          break;
        }
        // Number of input samples in this frame.
        int nb_samples = frame->nb_samples;
        // Prepare output buffer for conversion (mono channel, float).
        int out_channels = 1;
        float* out_buffer = new float[nb_samples * out_channels];
        uint8_t* out_data[1] = {reinterpret_cast<uint8_t*>(out_buffer)};

        // Convert samples.
        int converted_samples =
            swr_convert(swr_ctx, out_data, nb_samples, (const uint8_t**)frame->extended_data, nb_samples);
        if (converted_samples < 0) {
          std::cerr << "Error converting audio." << std::endl;
          delete[] out_buffer;
          break;
        }
        // Append the converted samples (only one channel) to our vector.
        for (int i = 0; i < converted_samples; i++) {
          audio_samples.push_back(static_cast<double>(out_buffer[i]));
          if (audio_samples.size() >= static_cast<size_t>(total_output_samples)) {
            finished = true;
            break;
          }
        }
        delete[] out_buffer;
        av_frame_unref(frame);
        if (finished)
          break;
      }
    }
    av_packet_unref(packet);
    if (finished)
      break;
  }

  // Clean up.
  av_packet_free(&packet);
  av_frame_free(&frame);
  swr_free(&swr_ctx);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);

  if (verbose) {
    std::cout << "Loaded audio from: " << file << ", samples: " << audio_samples.size()
              << ", sample rate: " << out_sample_rate << std::endl;
  }
  return {audio_samples, out_sample_rate};
}

// Get video FPS and duration (in seconds) using OpenCV.
std::pair<double, double> get_video_fps_and_duration(const std::string& video_path) {
  cv::VideoCapture cap(video_path);
  if (!cap.isOpened()) {
    std::cerr << "Could not open video: " << video_path << std::endl;
    std::exit(1);
  }
  double fps = cap.get(cv::CAP_PROP_FPS);
  int frame_count = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
  cap.release();
  double duration = frame_count / fps;
  return {fps, duration};
}

// Naïve cross-correlation between two signals (assumed to be the same length).
// Returns the lag (in samples) at which the correlation is maximum.
int cross_correlate(const std::vector<double>& x, const std::vector<double>& y) {
  size_t N = x.size();
  double max_corr = -1e308; // initialize to a very small number
  int best_lag = 0;

  // Lag range: from -(N-1) to N-1.
  for (int lag = -static_cast<int>(N) + 1; lag < static_cast<int>(N); ++lag) {
    double sum = 0.0;
    if (lag >= 0) {
      // For non-negative lag, sum over indices where both signals overlap.
      for (size_t i = 0; i < N - lag; ++i) {
        sum += x[i] * y[i + lag];
      }
    } else {
      // For negative lag.
      for (size_t i = 0; i < N + lag; ++i) {
        sum += x[i - lag] * y[i];
      }
    }
    if (sum > max_corr) {
      max_corr = sum;
      best_lag = lag;
    }
  }
  return best_lag;
}

// Synchronize two videos by comparing their audio tracks.
// Returns a pair (left_frame_offset, right_frame_offset) indicating the
// number of frames to skip in each video so that they are synchronized.
std::pair<int, int> synchronize_by_audio(
    const std::string& file1_path,
    const std::string& file2_path,
    double seconds = 15.0,
    bool verbose = true) {
  if (verbose) {
    std::cout << "Opening videos..." << std::endl;
  }
  // Retrieve FPS and duration for both videos.
  auto [video1_fps, video1_duration] = get_video_fps_and_duration(file1_path);
  auto [video2_fps, video2_duration] = get_video_fps_and_duration(file2_path);

  // Use a 0.5-second margin to ensure we don't exceed the available duration.
  seconds = std::min(seconds, std::min(video1_duration - 0.5, video2_duration - 0.5));

  double video1_subclip_frame_count = video1_fps * seconds;
  double video2_subclip_frame_count = video2_fps * seconds;

  if (verbose) {
    std::cout << "Loading audio..." << std::endl;
  }
  // Load audio. The stub load_audio_as_tensor should return a mono signal.
  auto [audio1, sample_rate1] = load_audio_as_tensor(file1_path, seconds, verbose);
  auto [audio2, sample_rate2] = load_audio_as_tensor(file2_path, seconds, verbose);

  // Calculate the number of audio samples per video frame.
  double audio_items_per_frame_1 = static_cast<double>(audio1.size()) / video1_subclip_frame_count;
  double audio_items_per_frame_2 = static_cast<double>(audio2.size()) / video2_subclip_frame_count;

  // Check that the computed samples per frame match the expected value.
  double expected_samples_per_frame1 = static_cast<double>(sample_rate1) / video1_fps;
  double expected_samples_per_frame2 = static_cast<double>(sample_rate2) / video2_fps;
  if (std::abs(expected_samples_per_frame1 - audio_items_per_frame_1) > 1e-3 ||
      std::abs(expected_samples_per_frame2 - audio_items_per_frame_2) > 1e-3) {
    std::cerr << "Mismatch in samples per frame calculation!" << std::endl;
    std::exit(1);
  }

  if (verbose) {
    std::cout << "Calculating cross-correlation..." << std::endl;
  }
  // Compute the cross-correlation using the first (or only) channel.
  int lag = cross_correlate(audio1, audio2);
  // The lag is already defined such that a positive lag means audio1 lags behind audio2.
  // Convert lag (in audio samples) to a frame offset.
  double frame_offset = lag / audio_items_per_frame_1;
  double time_offset = frame_offset / video1_fps;

  if (verbose) {
    std::cout << "Calculated frame offset: " << frame_offset << std::endl;
    std::cout << "Equivalent time offset: " << time_offset << " seconds" << std::endl;
  }

  // Determine the starting frame offset for each video.
  int left_frame_offset = (frame_offset > 0) ? static_cast<int>(std::round(frame_offset)) : 0;
  int right_frame_offset = (frame_offset < 0) ? static_cast<int>(std::round(-frame_offset)) : 0;

  return {left_frame_offset, right_frame_offset};
}

// Example usage:
int main() {
  std::string video1 = "video1.mp4";
  std::string video2 = "video2.mp4";
  auto [offset1, offset2] = synchronize_by_audio(video1, video2, 15.0, true);
  std::cout << "Left video offset (frames): " << offset1 << std::endl;
  std::cout << "Right video offset (frames): " << offset2 << std::endl;
  return 0;
}
