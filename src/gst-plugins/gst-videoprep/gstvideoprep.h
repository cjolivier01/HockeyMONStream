#pragma once

#include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"
#include "includes/hmcustomlib_interface.hpp"
#include "hstream/src/libs/common/Surface.h"

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video.h>

#include <cuda.h>
#include <npp.h>

#include "cupano/cuda/cudaStatus.h"

#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

#include "preputils.h"

#include "includes/hmcustomlib_base.hpp"
#include "videoprep_plugins.h"

#include <cassert>

#include "absl/status/statusor.h"
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

  bool render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
    return render_.render(name, surface, stream);
  }

  // -DSCustomLibraryBase
  bool SetProperty(const Property& prop) override {
    assert(false);
    return true;
  }

  bool HandleEvent(GstEvent* event) override {
    return true;
  }

  char* QueryProperties() override {
    assert(false);
    // ugh @ c programmers
    return strdup("");
  }

  BufferResult ProcessBuffer(GstBuffer* inbuf) override {
    assert(false);
    return BufferResult::Buffer_Ok;
  }

  // DSCustomLibraryBase-

  virtual absl::Status GenerateOutput(
      NvDsBatchMeta* batch_meta,
      videoprep::GstVideoPrep* videoprep,
      NvBufSurface* in_surface,
      NvBufSurface* out_surface) {
    assert(false);
    return absl::OkStatus();
  }

  virtual gint AllocateScratchBuffers(videoprep::GstVideoPrep* videoprep) {
    return 0;
  }

  void SetPrivateConfig(const char* config_string);

  GstFlowReturn get_last_flow_ret() const {
    return last_flow_ret_;
  }

 protected:
  GstFlowReturn last_flow_ret_{GST_FLOW_OK};
  RenderSet render_;
};

G_BEGIN_DECLS

/**
 * @addtogroup three Standard GStreamer boilerplate
 * @{
 */
#define GST_TYPE_VIDEOPREP (hm::videoprep::gst_videoprep_get_type())
#define GST_VIDEOPREP(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VIDEOPREP, videoprep::GstVideoPrep))
// #define GST_VIDEOPREP_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VIDEOPREP,
// videoprep::GstVideoPrepClass))
#define GST_IS_VIDEOPREP(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_VIDEOPREP))
#define GST_IS_VIDEOPREP_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_VIDEOPREP))

#define GST_VIDEOPREP_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_ELEMENT, videoprep::GstVideoPrepClass))

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

  guint input_batch_size;
  guint output_batch_size;

  hm::WHDims pre_rotate_size;

  guint num_batch_buffers;  /**< Number of batch buffers */
  guint gpu_id;             /**< ID of the GPU this element uses for dewarping/scaling. */

  gchar* config_file;             /**< String contaning path and name of configuration file */
  gchar* plugin_type;
  gchar* plugin_private_config;

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

  // gboolean deref_input_buffer;        /** defer the incoming GstBuffer */

  gboolean silent;                    /**< Boolean indicating swtiching on/off of verbose output */

  guint source_id;                            /**< Source ID of the input source */
  guint num_output_buffers;                   /**< Number of Output Buffers to be allocated by buffer pool */

  GstPadEventFunction parent_sink_event_fn;

  DSCustom_CreateParams custom_create_params;

  VideoPrepLibrary_Factory *priv_factory;
  VideoPrepPriv *priv;                       /**< Pointer to private data structure contaning dewarping parameters for all the output surfaces */
};
/* clang-format on */

/** GStreamer boilerplate. */
struct GstVideoPrepClass {
  GstBaseTransformClass parent_class;
  GstStateChangeReturn (*parent_change_state_fn)(GstElement* element, GstStateChange transition);
  void (*parent_state_changed_fn)(GstElement* element, GstState oldstate, GstState newstate, GstState pending);
};

GType gst_videoprep_get_type(void);
void videoprep_add_surface_meta(GstBuffer* out_gst_buf, int num_filled_surfaces, int source_id);

void gst_videoprep_class_init_base(GstVideoPrepClass* klass);
void gst_videoprep_init_base(GstVideoPrep* videoprep);

void gst_videoprep_hook_buffer_release(GstBuffer* buffer);

G_END_DECLS

} // namespace videoprep

} // namespace hm

/* clang-format on */
