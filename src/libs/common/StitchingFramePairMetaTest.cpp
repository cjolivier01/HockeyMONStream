#include "hstream/src/libs/common/StitchingFramePairMeta.h"

#include <dlfcn.h>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  using Add = bool (*)(NvDsFrameMeta*, const hm::StitchingFramePair*);
  using Read = bool (*)(const NvDsFrameMeta*, hm::StitchingFramePair*);
  const char* source_dir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  const std::string root = (source_dir ? std::string(source_dir) : std::string(argv[0]) + ".runfiles") + "/" +
      (workspace ? workspace : "kstream") + "/src/libs/common/";
  void* libraries[2]{};
  Add add[2]{};
  Read read[2]{};
  for (size_t index = 0; index < 2; ++index) {
    const auto path = root + "stitching_frame_pair_meta_dso_" + (index ? "b.so" : "a.so");
    libraries[index] = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!libraries[index]) {
      std::cerr << "Could not load pair metadata test library: " << dlerror() << '\n';
      return 1;
    }
    add[index] = reinterpret_cast<Add>(dlsym(libraries[index], "stitching_frame_pair_dso_add"));
    read[index] = reinterpret_cast<Read>(dlsym(libraries[index], "stitching_frame_pair_dso_read"));
    if (!add[index] || !read[index])
      return 1;
  }
  for (bool reuse_right : {false, true}) {
    NvDsBatchMeta* batch = nvds_create_batch_meta(2);
    NvDsBatchMeta* copied_batch = nvds_create_batch_meta(1);
    NvDsFrameMeta* left = nvds_acquire_frame_meta_from_pool(batch);
    NvDsFrameMeta* right = nvds_acquire_frame_meta_from_pool(batch);
    NvDsFrameMeta* copy = nvds_acquire_frame_meta_from_pool(copied_batch);
    nvds_add_frame_meta_to_batch(batch, left);
    nvds_add_frame_meta_to_batch(batch, right);
    nvds_add_frame_meta_to_batch(copied_batch, copy);
    const hm::StitchingFramePair pair{
        {0, 120, g_quark_from_static_string("file:///recording/cam1/chapter2.mp4"), 17 * GST_SECOND + 3},
        {1, 120, g_quark_from_static_string("file:///recording/cam2/chapter1.mp4"), 11 * GST_SECOND + 9},
        42 * GST_SECOND + 17,
        3840,
        2160,
        4096,
        2160};
    NvDsFrameMeta* reused = reuse_right ? right : left;
    if (!add[reuse_right](reused, &pair) || hm::add_stitching_frame_pair_meta(reused, pair)) {
      std::cerr << "Pair metadata must attach once to either retained input frame\n";
      return 1;
    }
    nvds_remove_frame_meta_from_batch(batch, reuse_right ? left : right);
    nvds_copy_frame_user_meta_list(reused->frame_user_meta_list, copy);
    nvds_destroy_batch_meta(batch);
    const auto actual = hm::stitching_frame_pair(copy);
    hm::StitchingFramePair cross_dso;
    const bool valid = read[!reuse_right](copy, &cross_dso) && actual &&
        cross_dso.left.source_pts == pair.left.source_pts && cross_dso.right.source_uri == pair.right.source_uri &&
        actual->left.source_id == 0 && actual->right.source_id == 1 && actual->left.sequence == 120 &&
        actual->right.sequence == 120 && actual->left.source_uri == pair.left.source_uri &&
        actual->right.source_uri == pair.right.source_uri && actual->left.source_pts == pair.left.source_pts &&
        actual->right.source_pts == pair.right.source_pts && actual->timeline_pts == pair.timeline_pts &&
        actual->left_width == 3840 && actual->right_width == 4096 && actual->left_height == 2160 &&
        actual->right_height == 2160;
    nvds_destroy_batch_meta(copied_batch);
    if (!valid) {
      std::cerr << "Both exact physical identities must survive input removal, batch copy, and original release\n";
      return 1;
    }
  }
  if (hm::stitching_frame_pair(nullptr) || hm::add_stitching_frame_pair_meta(nullptr, {}))
    return 1;
  for (void* library : libraries)
    dlclose(library);
  return 0;
}
