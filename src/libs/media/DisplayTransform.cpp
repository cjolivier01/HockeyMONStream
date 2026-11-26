#include "hstream/src/libs/media/DisplayMatrix.h"

#include <cstring>
extern "C" {
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
}

namespace hm::media {
absl::StatusOr<DisplayTransform> ParseDisplayMatrix(const int32_t matrix[9]) {
  if (!matrix[2] && !matrix[5] && matrix[8] > 0) {
    if (!matrix[1] && !matrix[3] && matrix[0] && matrix[4])
      return DisplayTransform{false, {matrix[0] < 0, matrix[4] < 0}};
    if (!matrix[0] && !matrix[4] && matrix[1] && matrix[3])
      return DisplayTransform{true, {matrix[3] < 0, matrix[1] < 0}};
  }
  return absl::UnimplementedError("Camera display matrix requires a non-orthogonal transform");
}

absl::StatusOr<DisplayTransform> StreamDisplayTransform(const AVStream& stream) {
// FFmpeg 6.1 already exposes the MOV matrix here at header-open time.
// Its legacy AVStream copy is populated only after probing packets.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 31, 102)
  const auto* side = av_packet_side_data_get(
      stream.codecpar->coded_side_data, stream.codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  const auto* bytes = side ? side->data : nullptr;
  const size_t size = side ? side->size : 0;
#else
  size_t size = 0;
  const auto* bytes = av_stream_get_side_data(&stream, AV_PKT_DATA_DISPLAYMATRIX, &size);
#endif
  if (!bytes)
    return DisplayTransform{};
  int32_t matrix[9];
  if (size < sizeof(matrix))
    return absl::DataLossError("Truncated camera display matrix");
  std::memcpy(matrix, bytes, sizeof(matrix));
  return ParseDisplayMatrix(matrix);
}
} // namespace hm::media
