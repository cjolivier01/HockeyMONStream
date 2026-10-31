# Camera source starts

Each selected camera has an Input tab beside Optics. **Source start** is a
position in that camera's complete chapter playlist, entered as
`HH:mm:ss.zzz`. Zero uses the beginning. Starts are applied before automatic
orientation, audio/IMU synchronization and calibration. Playback Start and
calibration Reference time are measured from the resulting synchronized timeline.

The two recordings need to show approximately the same event at their chosen
starts. Synchronization then refines the remaining difference. For caha-wave,
104 at 00:05 matches 105 near 01:47.646. Choosing 00:05 and 01:48 produces
absolute effective starts of 00:05.354146 and 01:48 with the audio matcher.
A weak match fails with a request to choose overlapping events instead of saving
an arbitrary offset.

## Configuration and ownership

```yaml
stitching:
  input_start_times:
    GX010104.MP4: 5
    GX010105.MP4: 108
```

`CameraSourceStart.*` validates at most two finite, nonnegative positions below
24 hours. Keys identify the first selected chapter, so a Left/Right reorder
cannot exchange trims. A map replaces the lower configuration layer as a whole;
zero entries canonicalize to absence. Unknown sources and duplicate path aliases
fail. Source replacement clears source-bound web settings. Account Defaults do
not retain recording-specific starts.

`game.stitching.frame_offsets` stores absolute effective skips in each camera's
own frame units. `game.stitching.input_start_times` records the map used to
calculate them. Old configs with no starts retain their existing meaning.
Changing a start invalidates offsets, manual matches, selected calibration
frames and output geometry. Experiments freeze the start map; promotion restores
it with the effective offsets. Player-rich candidates sharing one frame-count
group must share starts; incompatible saved frames are rejected.

## Seeking and backends

NVIDIA playback and synchronization use video-track chapter durations, excluding audio tails. NVIDIA playback skips complete chapters by that duration, requests the preceding
keyframe in the selected chapter, then discards decoded frames before the exact
start. If the seek fails, the existing exact-trim fallback decodes from that
chapter's beginning and logs the fallback. NVIDIA decoder restart after preroll
preserves its existing 8K HEVC workaround. Audio begins at the selected camera's
effective skip and is rebased to the synchronized timeline.

Audio synchronization seeks before decoding a window of at most 15 seconds.
IMU synchronization reads up to 35 seconds of positioned metadata; Insta360
trailers use timestamp binary search. Insta360 and GPMF windows exclude their ending timestamp, so concatenation cannot duplicate a chapter boundary. Short chapter slices combine before the
IMU correlation validation. Neither path decodes the skipped video.

AMD Program consumes the same saved effective offsets and start marker. Its
selected audio camera uses that camera's effective start for initial playback
and later seeks. Fresh AMD calibration remains subject to the existing gaps in
[amd-gpu.md](amd-gpu.md); this change does not claim to close them.

Qt/browser persistence, source retirement, native adapter and experiment
validation are covered by the corresponding UI, web and stitching tests.
