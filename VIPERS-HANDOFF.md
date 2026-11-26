# Vipers camera orientation handoff

**Native branch:** `fix/flipped-camera-order` in HockeyMONStream. The implementation is at `815208f8`. This handoff is the next commit on the same branch. Native PR: https://github.com/cjolivier01/HockeyMONStream/pull/426

**Web branch:** `fix/camera-display-orientation` in HockeyMONWeb. Commit `abf1167` pins the native implementation. Companion PR: https://github.com/cjolivier01/HockeyMONWeb/pull/149

## Resume on the other machine

From the primary native checkout, fetch the branch and create a sibling worktree. If the task branch already has a worktree on that machine, use that worktree and pull with `--ff-only`.

```sh
git fetch origin
git worktree add -b fix/flipped-camera-order --track ../hstream-flipped-camera-order origin/fix/flipped-camera-order
cd ../hstream-flipped-camera-order
git submodule update --init --recursive
```

**Remove this temporary handoff from Git once you have read or copied it.** Run `git rm VIPERS-HANDOFF.md`, commit the removal on `fix/flipped-camera-order`, and push. Leave both PRs open for review.

## Verified on the recording

The user saved `nahpl-vipers` with both flip boxes unchecked. All 16 GoPro chapters report −180° display rotation. In an isolated copy of that config, the rink masks assigned **Left GX010137.MP4** and **Right GX010131.MP4**. The integrated build produced 974 usable control points across four frame pairs, a 16371 × 4948 canvas and a successful playback restart. The saved stills matched upright FFmpeg views. The original game was untouched.

Apply the recording display matrix once per chapter, then apply the user's horizontal and vertical corrections in displayed coordinates. This order covers OpenCV rink inference, GPU preview, offline calibration stills and the GPU stitching inputs. The two 8K Vipers previews passed pixel comparisons for all four checkbox combinations and repeated seeks. A quarter-turn preview also passed. Earlier calibration on tagged recordings requires regeneration from Input.

## Outstanding work

1. **Investigate the Jetson crash on `stubby`.** At `358de29b`, `uri_multiple_pipeline_test --display-chapters-only` processed both chapter changes and reached EOS, then crashed during teardown. Under GDB the allocator reported `free(): invalid next size (fast)` on a queue thread. The backtrace ends in `tcache_thread_shutdown`. The corrupting write is still unidentified. Reproduce at `815208f8` with the two commands below. Compare the 180°-only and portrait-to-landscape cases if it persists. On x86 both the chapter rotation and one-to-four-input mux cases passed after merging current master. The production Vipers run passed on NVIDIA x86.
2. **Finish validation and review of `815208f8`.** The first review led to fixes for chapter dimensions, movie/track matrix composition, fisheye scale, Level Rink provenance and interrupted calibration. The next review found two further issues. The latest commit uses FFmpeg's MOV codec identifiers so calibration recognizes rotated H.264 `avc3` tracks, and shares display-coordinate caps checks between AMD and NVIDIA so an incompatible pixel aspect or chroma site cannot slip through on a quarter turn. The new caps and rotated-calibration regressions passed on x86. The AMD CLI built and the caps, VA playlist and HIP conversion tests passed on `ripper`. Full x86 and Jetson builds passed at the preceding integration commit. Verify the final builds and rerun the Jetson hardware regression. Request two fresh reviews of the resulting code before declaring it complete.
3. **Keep the web pin current** if native code changes. In the web worktree run `python3 tools/webapp/scripts/sync_stream_contract.py --hstream-root ../hstream-flipped-camera-order`, then run it again with `--check`. The 56 camera-start, native-preview and browser-playback tests passed on the integrated build. On the pre-merge native branch 307 of 309 default tests passed on the laptop. The two remaining tests, side-by-side encoding and the full Qt UI, both passed on `meshy`, where the full URI pipeline test also passed.

On `stubby`, build and run the hardware regression from the native worktree. The test generates its own media. The fixture FFmpeg must have both H.264 and HEVC encoders.

```sh
bazelisk build --config=opt --config=jetson //... //src/apps/apps-common:uri_multiple_pipeline_test
export PATH=/tmp/hstream-test-ffmpeg/bin:$PATH
export GST_PLUGIN_PATH="$PWD/bazel-bin/src/gst-plugins/gst-dsxvideoconvert:${GST_PLUGIN_PATH:-}"
bazel-bin/src/apps/apps-common/uri_multiple_pipeline_test --display-chapters-only
bazel-bin/src/apps/apps-common/uri_multiple_pipeline_test --mux-inputs-only
```

Logs are copied to `meshy:~/src/vipers-orientation-handoff/`, including the real-game `cli.log`, calibration still comparison, AMD test results and Jetson backtrace. The isolated copy used for the integrated run is `/mnt/data/src/.vipers-display-dOyOx5/nahpl-vipers` on `legion`. To reproduce the game run elsewhere, create a fresh games root, copy only the original game's `_hockeymon/config.yaml`, and link its 16 original MP4s into the new `nahpl-vipers` directory. Set `HM_GAME_DIR` to that fresh root and `HM_CONFIG_ROOT` to the worktree's `configs` directory, then run:

```sh
bazel-bin/src/apps/hstream-cli/hstream-cli -g nahpl-vipers --stitching-calibration-only --enable-sources=URI-MULTIPLE --enable-sinks=FAKE -t 1
```
