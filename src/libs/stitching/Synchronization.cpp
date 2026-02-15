#include "hstream/src/libs/stitching/Synchronization.h"
#include "hstream/src/libs/stitching/CrossCorrelation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

#include <opencv2/opencv.hpp>

// FFmpeg headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
namespace hm {
namespace stitching {
namespace {
/**
 * @brief Loads audio from a file (typically an MP4) using FFmpeg and returns a waveform
 *        along with its sample rate.
 *
 * The waveform is returned as a two-dimensional vector of doubles with shape
 * [channels][samples]. For example, if the input audio is stereo and you read 720000 samples per channel,
 * then the waveform will have two vectors of length 720000.
 *
 * @param file The path to the audio (or video) file.
 * @param duration_seconds The duration (in seconds) to read from the audio.
 * @param verbose If true, prints debug information.
 * @return A pair containing the waveform and the sample rate.
 */
#if 1
std::pair<std::vector<std::vector<float>>, int> load_audio_as_tensor(
    const std::string& file,
    double duration_seconds,
    bool verbose = false) {
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

  // Determine the input channel layout (FFmpeg 5+ uses AVChannelLayout).
  AVChannelLayout in_ch_layout{};
  if (codecpar->ch_layout.nb_channels > 0) {
    if (av_channel_layout_copy(&in_ch_layout, &codecpar->ch_layout) < 0) {
      std::cerr << "Could not copy channel layout." << std::endl;
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      std::exit(1);
    }
  } else if (codec_ctx->ch_layout.nb_channels > 0) {
    if (av_channel_layout_copy(&in_ch_layout, &codec_ctx->ch_layout) < 0) {
      std::cerr << "Could not copy channel layout from codec context." << std::endl;
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      std::exit(1);
    }
  } else {
    // Last-ditch fallback: assume stereo.
    av_channel_layout_default(&in_ch_layout, 2);
  }

  // Determine number of channels (preserve original channels).
  const int out_channels = in_ch_layout.nb_channels;

  // Set up the resampler to convert the audio to interleaved float format while preserving channel count.
  SwrContext* swr_ctx = nullptr;
  AVChannelLayout out_ch_layout{};
  if (av_channel_layout_copy(&out_ch_layout, &in_ch_layout) < 0) {
    std::cerr << "Could not copy output channel layout." << std::endl;
    av_channel_layout_uninit(&in_ch_layout);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }

  if (swr_alloc_set_opts2(
          &swr_ctx,
          &out_ch_layout,
          AV_SAMPLE_FMT_FLT,
          codec_ctx->sample_rate,
          &in_ch_layout,
          codec_ctx->sample_fmt,
          codec_ctx->sample_rate,
          0,
          nullptr) < 0 ||
      !swr_ctx) {
    std::cerr << "Could not allocate/configure resampler context." << std::endl;
    av_channel_layout_uninit(&out_ch_layout);
    av_channel_layout_uninit(&in_ch_layout);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }

  if (swr_init(swr_ctx) < 0) {
    std::cerr << "Could not initialize resampler." << std::endl;
    swr_free(&swr_ctx);
    av_channel_layout_uninit(&out_ch_layout);
    av_channel_layout_uninit(&in_ch_layout);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }

  int out_sample_rate = codec_ctx->sample_rate;
  // Total output samples per channel.
  int64_t total_output_samples = static_cast<int64_t>(duration_seconds * out_sample_rate);

  // Create a 2D vector for storing output audio: one vector per channel.
  std::vector<std::vector<float>> audio_samples(out_channels);
  for (int ch = 0; ch < out_channels; ++ch) {
    audio_samples[ch].reserve(total_output_samples);
  }

  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool finished = false;

  // Decode and convert audio frames.
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
        int nb_samples = frame->nb_samples; // number of samples per channel in this frame

        // Allocate buffer for converted data: converted samples are interleaved.
        int max_out_samples = nb_samples; // swr_convert returns samples per channel.
        int buffer_size = max_out_samples * out_channels;
        float* out_buffer = new float[buffer_size];
        uint8_t* out_data[1] = {reinterpret_cast<uint8_t*>(out_buffer)};

        int converted_samples =
            swr_convert(swr_ctx, out_data, max_out_samples, (const uint8_t**)frame->extended_data, nb_samples);
        if (converted_samples < 0) {
          std::cerr << "Error converting audio." << std::endl;
          delete[] out_buffer;
          break;
        }
        // Deinterleave the interleaved output into separate channels.
        for (int i = 0; i < converted_samples; i++) {
          for (int ch = 0; ch < out_channels; ch++) {
            audio_samples[ch].push_back(out_buffer[i * out_channels + ch]);
          }
        }
        delete[] out_buffer;
        av_frame_unref(frame);
        // Check if we have reached the desired sample count for each channel.
        if (audio_samples[0].size() >= static_cast<size_t>(total_output_samples)) {
          finished = true;
          break;
        }
      }
    }
    av_packet_unref(packet);
    if (finished)
      break;
  }

  av_packet_free(&packet);
  av_frame_free(&frame);
  swr_free(&swr_ctx);
  av_channel_layout_uninit(&out_ch_layout);
  av_channel_layout_uninit(&in_ch_layout);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);

  if (verbose) {
    std::cout << "Loaded audio from: " << file << ", channels: " << out_channels
              << ", samples per channel: " << audio_samples[0].size() << ", sample rate: " << out_sample_rate
              << std::endl;
  }
  return {audio_samples, out_sample_rate};
}
#else
std::pair<std::vector<std::vector<float>>, int> load_audio_as_tensor(
    const std::string& file,
    double duration_seconds,
    bool verbose = false) {
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

  // Determine the input channel layout.
  uint64_t in_channel_layout = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 61
  uint64_t param_layout = av_codec_parameters_get_channel_layout(codecpar);
#else
  uint64_t param_layout = codecpar->channel_layout;
#endif
  if (param_layout != 0) {
    in_channel_layout = param_layout;
  } else {
#if LIBAVCODEC_VERSION_MAJOR >= 61
    int channels = av_codec_parameters_get_channels(codecpar);
#else
    int channels = codecpar->channels;
#endif
    AVChannelLayout default_layout;
    av_channel_layout_default(&default_layout, channels);
    in_channel_layout = default_layout.u.mask;
  }

  // Determine number of channels (preserve original channels).
#if LIBAVCODEC_VERSION_MAJOR >= 61
  int out_channels = av_codec_parameters_get_channels(codecpar);
#else
  int out_channels = codecpar->channels;
#endif

  // Set up the resampler to convert the audio to float format while preserving channel count.
  SwrContext* swr_ctx = swr_alloc();
  if (!swr_ctx) {
    std::cerr << "Could not allocate resampler context." << std::endl;
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }
  // Input options.
  av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);
  // Output options: preserve number of channels.
  av_opt_set_int(swr_ctx, "out_channel_layout", in_channel_layout, 0);
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
  // Total output samples per channel.
  int64_t total_output_samples = static_cast<int64_t>(duration_seconds * out_sample_rate);

  // Create a 2D vector for storing output audio: one vector per channel.
  std::vector<std::vector<float>> audio_samples(out_channels);
  for (int ch = 0; ch < out_channels; ++ch) {
    audio_samples[ch].reserve(total_output_samples);
  }

  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool finished = false;

  // Decode and convert audio frames.
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
        int nb_samples = frame->nb_samples; // number of samples per channel in this frame

        // Allocate buffer for converted data: note that converted samples are interleaved.
        // We request conversion for nb_samples.
        int max_out_samples = nb_samples; // swr_convert returns samples per channel.
        int buffer_size = max_out_samples * out_channels;
        float* out_buffer = new float[buffer_size];
        uint8_t* out_data[1] = {reinterpret_cast<uint8_t*>(out_buffer)};

        int converted_samples =
            swr_convert(swr_ctx, out_data, max_out_samples, (const uint8_t**)frame->extended_data, nb_samples);
        if (converted_samples < 0) {
          std::cerr << "Error converting audio." << std::endl;
          delete[] out_buffer;
          break;
        }
        // Deinterleave the interleaved output into separate channels.
        for (int i = 0; i < converted_samples; i++) {
          for (int ch = 0; ch < out_channels; ch++) {
            audio_samples[ch].push_back(static_cast<double>(out_buffer[i * out_channels + ch]));
          }
        }
        delete[] out_buffer;
        av_frame_unref(frame);
        // Check if we have reached the desired sample count for each channel.
        if (audio_samples[0].size() >= static_cast<size_t>(total_output_samples)) {
          finished = true;
          break;
        }
      }
    }
    av_packet_unref(packet);
    if (finished)
      break;
  }

  av_packet_free(&packet);
  av_frame_free(&frame);
  swr_free(&swr_ctx);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);

  if (verbose) {
    std::cout << "Loaded audio from: " << file << ", channels: " << out_channels
              << ", samples per channel: " << audio_samples[0].size() << ", sample rate: " << out_sample_rate
              << std::endl;
  }
  return {audio_samples, out_sample_rate};
}
#endif

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

/**
 * @brief Computes the lag corresponding to the maximum correlation.
 *
 * This mimics the Python code:
 * @code
 * lag = np.argmax(correlation) - audio1.shape[0] + 1
 * @endcode
 *
 * @param correlation The full cross-correlation vector.
 * @param audio1_size The size of the first signal.
 * @return int The computed lag.
 */
int get_lag(const std::vector<float>& correlation, size_t audio1_size) {
  // Find the index of the maximum element in the correlation vector.
  auto max_iter = std::max_element(correlation.begin(), correlation.end());
  int max_index = std::distance(correlation.begin(), max_iter);
  // Compute lag such that the zero lag is at index audio1_size - 1.
  int lag = max_index - static_cast<int>(audio1_size) + 1;
  return lag;
}

double sum(const std::vector<float>& array) {
  double sum1 = 0;
  std::for_each(array.begin(), array.end(), [&sum1](auto v) { sum1 += v; });
  return sum1;
}

} // namespace

// Synchronize two videos by comparing their audio tracks.
// Returns a pair (left_frame_offset, right_frame_offset) indicating the
// number of frames to skip in each video so that they are synchronized.
std::pair<double, double> synchronize_by_audio(
    const std::string& file1_path,
    const std::string& file2_path,
    double seconds,
    bool verbose) {
  if (verbose) {
    std::cout << "Opening videos..." << std::endl;
  }
  // Retrieve FPS and duration for both videos.
  auto [video1_fps, video1_duration] = get_video_fps_and_duration(file1_path);
  auto [video2_fps, video2_duration] = get_video_fps_and_duration(file2_path);

  // Use a 0.5-second margin to ensure we don't exceed the available duration.
  seconds = std::min(seconds, std::min(video1_duration - 0.5, video2_duration - 0.5));

  const double video1_subclip_frame_count = video1_fps * seconds;
  const double video2_subclip_frame_count = video2_fps * seconds;

  if (verbose) {
    std::cout << "Loading audio..." << std::endl;
  }
  // Load audio. The load_audio_as_tensor returns a mono signal.
  auto [audio1, sample_rate1] = load_audio_as_tensor(file1_path, seconds, verbose);
  auto [audio2, sample_rate2] = load_audio_as_tensor(file2_path, seconds, verbose);

  double sum1 = sum(audio1[0]);
  double sum2 = sum(audio2[0]);

  // Calculate the number of audio samples per video frame.
  const double audio_items_per_frame_1 = static_cast<double>(audio1[0].size()) / video1_subclip_frame_count;
  const double audio_items_per_frame_2 = static_cast<double>(audio2[0].size()) / video2_subclip_frame_count;

  // Check that the computed samples per frame match the expected value.
  // const double expected_samples_per_frame1 = static_cast<double>(sample_rate1) / video1_fps;
  // const double expected_samples_per_frame2 = static_cast<double>(sample_rate2) / video2_fps;
  // if (std::abs(expected_samples_per_frame1 - audio_items_per_frame_1) > 1e-3 ||
  //     std::abs(expected_samples_per_frame2 - audio_items_per_frame_2) > 1e-3) {
  //   std::cerr << "Mismatch in samples per frame calculation!" << std::endl;
  //   std::exit(1);
  // }

  if (verbose) {
    std::cout << "Calculating cross-correlation..." << std::endl;
  }
  // Compute the cross-correlation using the first (or only) channel.
  // std::vector<float> correlation = full_correlate(audio1[0], audio2[0]);

  std::vector<float> correlation = full_correlate_fft(audio1[0], audio2[0]);

  auto sumc = sum(correlation);

  int lag = -get_lag(correlation, audio1[0].size());

  // A positive lag means audio1 lags behind audio2.
  const double frame_offset = lag / audio_items_per_frame_1;
  const double time_offset = frame_offset / video1_fps;

  if (verbose) {
    std::cout << "Calculated frame offset: " << frame_offset << std::endl;
    std::cout << "Equivalent time offset: " << time_offset << " seconds" << std::endl;
  }

  // Determine the starting frame offset for each video.
  const double left_frame_offset = (frame_offset > 0.0) ? frame_offset : 0.0;
  const double right_frame_offset = (frame_offset < 0.0) ? -frame_offset : 0.0;

  if (verbose) {
    std::cout << "Left frame offset: " << left_frame_offset << std::endl;
    std::cout << "Right time offset: " << right_frame_offset << std::endl;
  }

  return {left_frame_offset, right_frame_offset};
}

} // namespace stitching
} // namespace hm
