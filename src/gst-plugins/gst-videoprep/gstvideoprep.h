#pragma once

#include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"
#include "src/libs/common/Surface.h"

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video.h>

#include <cuda.h>
#include <npp.h>

#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

#include "preputils.h"

#include "includes/hmcustomlib_base.hpp"
#include "videoprep_plugins.h"

#include <cassert>

namespace hm {
namespace videoprep {

#define DISTORTION_SIZE 5 /**< Maximum number of distortion coefficients */
#define FOCAL_LENGTH_SIZE 2 /**< Focal length array size : two values for X & Y direction */
#define ROTATION_MATRIX_SIZE 9 /**< Standard rotation matrix size */

/** Data structure contaning dewarping parameters for all the output surfaces */
class VideoPrepPriv : public DSCustomLibraryBase {
 public:
  VideoPrepPriv(int gpu_id, size_t batch_size) : scratch_buffers(gpu_id, batch_size) {}
  hm::surface::SurfaceList scratch_buffers;

  // template <typename... Args>
  // void render(Args&&... args) {
  //     // Forward all arguments to the target function
  //     render_(std::forward<Args>(args)...);
  // }

  void render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
    return render_.render(name, surface, stream);
  }

  // -DSCustomLibraryBase
  bool SetProperty(Property& prop) override {
    assert(false);
    return true;
  }

  bool HandleEvent(GstEvent* event) override {
    assert(false);
    return true;
  }

  const char* QueryProperties() override {
    assert(false);
    return "";
  }

  BufferResult ProcessBuffer(GstBuffer* inbuf) override {
    assert(false);
    return BufferResult::Buffer_Ok;
  }

  // DSCustomLibraryBase-

  virtual cudaError GenerateOutput(
      NvDsBatchMeta* batch_meta,
      videoprep::GstVideoPrep* videoprep,
      NvBufSurface* in_surface,
      NvBufSurface* out_surface) {
    assert(false);
    return cudaError_t::cudaSuccess;
  }

 protected:
  RenderSet render_;
};

G_BEGIN_DECLS

/**
 * @addtogroup three Standard GStreamer boilerplate
 * @{
 */
#define GST_TYPE_VIDEOPREP (hm::videoprep::gst_videoprep_get_type())
#define GST_VIDEOPREP(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VIDEOPREP, GstVideoPrep))
#define GST_VIDEOPREP_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VIDEOPREP, GstVideoPrepClass))
#define GST_IS_VIDEOPREP(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_VIDEOPREP))
#define GST_IS_VIDEOPREP_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_VIDEOPREP))

/**
 * GstVideoPrep element structure.
 */
/* clang-format off */
struct GstVideoPrep
{
  GstBaseTransform element; /**< Should be the first member when extending from GstBaseTransform. */

  GstCaps *sinkcaps;        /**< Sink pad caps */
  GstCaps *srccaps;         /**< Source pad caps */

  guint input_width;        /**<Input frame width */
  guint input_height;       /**<Input frame height */
  guint output_width;       /**<Output frame width */
  guint output_height;      /**<Output frame height */

  hm::WHDims pre_rotate_size;

  guint num_batch_buffers;  /**< Number of batch buffers */
  guint gpu_id;             /**< ID of the GPU this element uses for dewarping/scaling. */

  gchar* config_file;             /**< String contaning path and name of configuration file */
  gchar* plugin_type;

  GstBufferPool *pool;            /**< Internal buffer pool for output buffers  */

  /** Input memory feature can take values MEM_FEATURE_NVMM/MEM_FEATURE_RAW
   * based on input  memory type caps*/
  gint input_feature;
  /** Output memory feature can take values MEM_FEATURE_NVMM/MEM_FEATURE_RAW
   * based on output  memory type caps*/
  gint output_feature;

  NvBufSurfaceMemType cuda_mem_type;              /**< Cuda surface memory type set by "nvbuf-memory-type" */
  NvBufSurfTransform_Inter interpolation_method;  /**< Interpolation method for scaling. Set by config param "interpolation-method" */
  GstVideoFormat input_fmt;                       /**< Input stream format derived from sink caps */
  GstVideoFormat output_fmt;                      /**< Output stream format derived from src caps */

  cudaStream_t stream;                            /**< Cuda Stream to launch operations on. */

  guint frame_num;                                /**< Number of the frame in the stream that was last processed. */

  guint dump_frames;  /**< Number of dewarped output frames to be dumped in a *.rgba file. Useful for debugging */
  void *output;       /**< Host memory  pointer for output buffer. Used for frame dumps. */

  gboolean silent;                    /**< Boolean indicating swtiching on/off of verbose output */

  guint source_id;                            /**< Source ID of the input source */
  guint num_output_buffers;                   /**< Number of Output Buffers to be allocated by buffer pool */

  GstBuffer * out_gst_buf;                    /**< Pointer to the output buffer */

  VideoPrepLibrary_Factory *priv_factory;
  VideoPrepPriv *priv;                       /**< Pointer to private data structure contaning dewarping parameters for all the output surfaces */
};
/* clang-format on */

/** GStreamer boilerplate. */
struct GstVideoPrepClass {
  GstBaseTransformClass parent_class;
};

GType gst_videoprep_get_type(void);
void videoprep_add_surface_meta(GstBuffer* out_gst_buf, int num_filled_surfaces, int source_id);

void gst_videoprep_class_init_base(GstVideoPrepClass* klass);
void gst_videoprep_init_base(GstVideoPrep* videoprep);

G_END_DECLS

} // namespace videoprep

} // namespace hm

/* clang-format on */
