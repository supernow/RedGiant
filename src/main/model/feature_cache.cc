#include "model/feature_cache.h"

#include <memory>
#include <utility>

#include "model/feature.h"
#include "model/feature_space.h"

namespace redgiant {

auto FeatureCache::create_or_get_feature(const std::string& feature_key,
    const std::string& space_name)
-> std::shared_ptr<Feature> {
  auto iter = spaces_.find(space_name);
  if (iter == spaces_.end()) {
    // space not found
    return nullptr;
  }
  return create_or_get_feature(feature_key, iter->second);
}

auto FeatureCache::create_or_get_feature(const std::string& feature_key,
    const std::shared_ptr<FeatureSpace>& space)
-> std::shared_ptr<Feature> {
  FeatureId id = space->calculate_feature_id(feature_key);
  if (id == FeatureSpace::kInvalidId) {
    return nullptr;
  }

  auto iter = features_.find(id);
  if (iter != features_.end()) {
    return iter->second;
  } else {
    std::shared_ptr<Feature> feature = std::make_shared<Feature>(feature_key, id);
    // add the newly created feature
    features_[id] = feature;
    return feature;
  }
}

} /* namespace redgiant */
