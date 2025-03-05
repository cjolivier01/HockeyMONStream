#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

#include <fftw3.h>
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

namespace {
// Loads audio from a file (or video file) using FFmpeg and returns a mono audio signal
// as a vector of doubles along with its sample rate.
std::pair<std::vector<double>, int> load_audio_as_tensor(
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
    // av_channel_layout_default fills default_layout based on the number of channels.
    av_channel_layout_default(&default_layout, channels);
    in_channel_layout = default_layout.u.mask;
  }

  // Set up the resampler to convert the audio to mono, float format.
  SwrContext* swr_ctx = swr_alloc();
  if (!swr_ctx) {
    std::cerr << "Could not allocate resampler context." << std::endl;
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    std::exit(1);
  }
  av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);
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
  int64_t total_output_samples = static_cast<int64_t>(duration_seconds * out_sample_rate);
  std::vector<double> audio_samples;
  audio_samples.reserve(total_output_samples);

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
        int nb_samples = frame->nb_samples;
        int out_channels = 1;
        float* out_buffer = new float[nb_samples * out_channels];
        uint8_t* out_data[1] = {reinterpret_cast<uint8_t*>(out_buffer)};

        int converted_samples =
            swr_convert(swr_ctx, out_data, nb_samples, (const uint8_t**)frame->extended_data, nb_samples);
        if (converted_samples < 0) {
          std::cerr << "Error converting audio." << std::endl;
          delete[] out_buffer;
          break;
        }
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

// Computes the full cross-correlation of two real-valued signals x and y using FFTW,
// and returns the lag (ranging from -(N-1) to N-1) corresponding to the maximum correlation.
int cross_correlate_fft(const std::vector<double>& x, const std::vector<double>& y) {
  // Assume x and y have the same length.
  size_t N = x.size();
  // The length of the linear convolution (cross-correlation) is L = 2*N - 1.
  size_t L = 2 * N - 1;

  // Allocate arrays for padded input signals.
  std::vector<double> x_pad(L, 0.0);
  std::vector<double> y_pad(L, 0.0);
  for (size_t i = 0; i < N; ++i) {
    x_pad[i] = x[i];
    y_pad[i] = y[i];
  }

  // Allocate output arrays for FFT: FFTW computes real-to-complex with output size L/2+1.
  size_t Nc = L / 2 + 1;
  fftw_complex* X = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * Nc);
  fftw_complex* Y = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * Nc);
  fftw_complex* C = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * Nc);

  // Create FFTW plans for forward transforms.
  fftw_plan plan_x = fftw_plan_dft_r2c_1d(L, x_pad.data(), X, FFTW_ESTIMATE);
  fftw_plan plan_y = fftw_plan_dft_r2c_1d(L, y_pad.data(), Y, FFTW_ESTIMATE);

  fftw_execute(plan_x);
  fftw_execute(plan_y);

  // Compute the product: X * conj(Y) for each frequency bin.
  for (size_t i = 0; i < Nc; ++i) {
    double a = X[i][0], b = X[i][1]; // X = a + ib
    double c = Y[i][0], d = Y[i][1]; // Y = c + id
    // X * conj(Y) = (a + ib) * (c - id) = (a*c + b*d) + i(b*c - a*d)
    C[i][0] = a * c + b * d;
    C[i][1] = b * c - a * d;
  }

  // Allocate array for the inverse FFT result (the cross-correlation).
  std::vector<double> corr(L, 0.0);
  fftw_plan plan_corr = fftw_plan_dft_c2r_1d(L, C, corr.data(), FFTW_ESTIMATE);
  fftw_execute(plan_corr);

  // FFTW does not normalize the inverse FFT, so divide by L.
  for (size_t i = 0; i < L; ++i) {
    corr[i] /= static_cast<double>(L);
  }

  // The result is arranged such that index (N-1) corresponds to zero lag.
  int best_lag = 0;
  double max_corr = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < L; ++i) {
    int lag = static_cast<int>(i) - static_cast<int>(N - 1);
    if (corr[i] > max_corr) {
      max_corr = corr[i];
      best_lag = lag;
    }
  }

  fftw_destroy_plan(plan_x);
  fftw_destroy_plan(plan_y);
  fftw_destroy_plan(plan_corr);
  fftw_free(X);
  fftw_free(Y);
  fftw_free(C);

  return best_lag;
}

} // namespace

// Synchronize two videos by comparing their audio tracks.
// Returns a pair (left_frame_offset, right_frame_offset) indicating the
// number of frames to skip in each video so that they are synchronized.
std::pair<int, int> synchronize_by_audio(
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

  double video1_subclip_frame_count = video1_fps * seconds;
  double video2_subclip_frame_count = video2_fps * seconds;

  if (verbose) {
    std::cout << "Loading audio..." << std::endl;
  }
  // Load audio. The load_audio_as_tensor returns a mono signal.
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
  int lag = cross_correlate_fft(audio1, audio2);
  // A positive lag means audio1 lags behind audio2.
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
// int main() {
//   std::string video1 = "video1.mp4";
//   std::string video2 = "video2.mp4";
//   auto [offset1, offset2] = synchronize_by_audio(video1, video2, 15.0, true);
//   std::cout << "Left video offset (frames): " << offset1 << std::endl;
//   std::cout << "Right video offset (frames): " << offset2 << std::endl;
//   return 0;
// }
