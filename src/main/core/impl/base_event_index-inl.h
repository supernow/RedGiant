#ifndef SRC_MAIN_CORE_IMPL_BASE_EVENT_INDEX_INL_H_
#define SRC_MAIN_CORE_IMPL_BASE_EVENT_INDEX_INL_H_

#include "core/impl/base_event_index.h"
#include "core/impl/base_index.h"
#include "core/impl/base_index-inl.h"

namespace redgiant {

template <typename DocTraits>
template <typename Loader>
BaseEventIndex<DocTraits>::BaseEventIndex(size_t initial_buckets, size_t max_size, Loader&& loader)
: Base(initial_buckets, loader), max_size_(max_size), expire_(loader) {
}

template <typename DocTraits>
size_t BaseEventIndex<DocTraits>::get_expire_table_size() const {
  std::unique_lock<std::mutex> lock_change(changeset_mutex_);
  return expire_.size();
}

template <typename DocTraits>
int BaseEventIndex<DocTraits>::update(DocId doc_id, TermId term_id, const TermWeight& weights, ExpireTime expire_time) {
  std::unique_lock<std::mutex> lock_change(changeset_mutex_);
  update_expire_internal(doc_id, term_id, expire_time);
  return create_update_internal(doc_id, term_id, weights, changeset_);
}

template <typename DocTraits>
int BaseEventIndex<DocTraits>::batch_update(const std::vector<EventTuple>& batch) {
  int ret = 0;
  std::unique_lock<std::mutex> lock_change(changeset_mutex_);
  for (const EventTuple& tuple: batch) {
    update_expire_internal(std::get<0>(tuple), std::get<1>(tuple), std::get<3>(tuple));
    ret += create_update_internal(std::get<0>(tuple), std::get<1>(tuple), std::get<2>(tuple), changeset_);
  }
  return ret;
}

template <typename DocTraits>
std::pair<int, int> BaseEventIndex<DocTraits>::apply(ExpireTime expire_time) {
  int ret_expire = 0;
  int ret = 0;
  std::unique_lock<std::mutex> lock_change(changeset_mutex_);
  ExpVec results = expire_.expire_with_limit(expire_time, max_size_);
  ret_expire += results.size();
  for (auto& expire_item: results) {
    remove_internal(expire_item.first.doc_id, expire_item.first.term_id, changeset_);
  }
  ret += apply_internal(changeset_);
  return std::make_pair(ret, ret_expire);
}

template <typename DocTraits>
template <typename Dumper>
size_t BaseEventIndex<DocTraits>::dump(Dumper&& dumper) {
  size_t ret = 0;
  ret += dump_internal(dumper);
  ret += expire_.dump(dumper);
  return ret;
}

template <typename DocTraits>
void BaseEventIndex<DocTraits>::update_expire_internal(DocId doc_id, TermId term_id, ExpireTime expire_time) {
  expire_.update({term_id, doc_id}, expire_time);
}

} /* namespace redgiant */

#endif /* SRC_MAIN_CORE_IMPL_BASE_EVENT_INDEX_INL_H_ */
