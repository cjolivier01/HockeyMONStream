#pragma once

// #include "BYTETracker.h"
#include "nvdstracker.h"

#include <map>
#include <memory>

#define MAX_TARGETS_PER_STREAM 512

class BYTETracker;

/**
 * @brief Context for input video streams
 *
 * The stream context holds all necessary state to perform multi-object tracking
 * within the stream.
 *
 */
class NvMOTContext {
 public:
  NvMOTContext(const NvMOTConfig& configIn, NvMOTConfigResponse& configResponse);

  ~NvMOTContext();

  /**
   * @brief Process a batch of frames
   *
   * Internal implementation of NvMOT_Process()
   *
   * @param [in] pParam Pointer to parameters for the frame to be processed
   * @param [out] pTrackedObjectsBatch Pointer to object tracks output
   */
  NvMOTStatus processFrame(const NvMOTProcessParams* params, NvMOTTrackedObjBatch* pTrackedObjectsBatch);
  /**
   * @brief Output the miscellaneous data if there are
   *
   *  Internal implementation of retrieveMiscData()
   *
   * @param [in] pParam Pointer to parameters for the frame to be processed
   * @param [out] pTrackerMiscData Pointer to miscellaneous data output
   */
  NvMOTStatus retrieveMiscData(const NvMOTProcessParams* params, NvMOTTrackerMiscData* pTrackerMiscData);
  /**
   * @brief Terminate trackers and release resources for a stream when the stream is removed
   *
   *  Internal implementation of NvMOT_RemoveStreams()
   *
   * @param [in] streamIdMask removed stream ID
   */
  NvMOTStatus removeStream(const NvMOTStreamId streamIdMask);

 protected:
  /**
   * Users can include an actual tracker implementation here as a member
   * `IMultiObjectTracker` can be assumed to an user-defined interface class
   */
  std::map<uint64_t, std::shared_ptr<BYTETracker>> byteTrackerMap;
};
