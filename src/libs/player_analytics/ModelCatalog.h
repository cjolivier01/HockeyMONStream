#pragma once

#include <string_view>

namespace hm::player_analytics {

// Portable model contracts only. Looking up a selection does not inspect the
// filesystem, load an SDK, create a GPU context, or prepare a model.
struct CatalogModel {
  const char* feature;
  const char* id;
  const char* label;
  const char* onnx_url;
  const char* onnx_sha256;
  const char* contract_json;
  int max_batch;
};

std::string_view DefaultModelId(std::string_view feature);
const CatalogModel* FindModel(std::string_view feature, std::string_view id);

} // namespace hm::player_analytics
