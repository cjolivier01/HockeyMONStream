#pragma once

namespace hm {
namespace videoprep {

/* clang-format off */

// Configuration keys for the video preparation property group.
constexpr const char* CONFIG_GROUP_VIDEOPREP_PROPERTY = "property";                                   // Identifier for [property] group
constexpr const char* CONFIG_GROUP_VIDEOPREP_PROPERTY_CONFIG_FILE = "config-file";
constexpr const char* CONFIG_GROUP_VIDEOPREP_PROPERTY_OUTPUT_WIDTH = "output-width";                  // Scale dewarped surfaces to specified output width
constexpr const char* CONFIG_GROUP_VIDEOPREP_PROPERTY_OUTPUT_HEIGHT = "output-height";                // Scale dewarped surfaces to specified output height
constexpr const char* CONFIG_GROUP_VIDEOPREP_PROPERTY_CUDA_MEMORY_TYPE = "cuda-memory-type";          // NVDS CUDA memory type
constexpr const char* CONFIG_GROUP_VIDEOPREP_PROPERTY_NUM_BATCH_BUFFERS = "num-batch-buffers";        // Number of dewarped output surfaces per buffer
constexpr const char* CONFIG_GROUP_VIDEOPREP_PROPERTY_DUMP_FRAMES = "dewarp-dump-frames";             // Number of dewarped frames to dump

/**
 * @addtogroup two Surface properties
 *
 * @brief Surface properties which can be different for every surface.
 * These are specified under [surface<n>] group
 * @{
 */
constexpr const char* CONFIG_GROUP_VIDEOPREP_SURFACE_ATTRS_PREFIX = "surface";                // Identifier for [surface<n>] group
constexpr const char* CONFIG_GROUP_VIDEOPREP_SURFACE_INDEX = "surface-index";                 // An index that distinguishes surfaces of the same projection type.
constexpr const char* CONFIG_GROUP_VIDEOPREP_SURFACE_ADDRESS_MODE = "cuda-address-mode";      // CUDA Texture addressing mode.
constexpr const char* CONFIG_GROUP_VIDEOPREP_PLUGIN_TYPE = "plugin-type";
constexpr const char* CONFIG_GROUP_VIDEOPREP_PLUGIN_PRIVATE_CONFIG = "plugin-private-config";

/* clang-format on */

} // namespace videoprep
} // namespace hm
