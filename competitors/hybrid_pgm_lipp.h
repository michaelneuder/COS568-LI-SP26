#pragma once

#include <algorithm>
#include <vector>

#include "../util.h"
#include "base.h"
#include "pgm_index_dynamic.hpp"
#include "./lipp/src/core/lipp.h"

// Hybrid index: uses DynamicPGM as a write buffer and LIPP as the read-optimized store.
// Inserts go into the DPGM buffer. When the buffer reaches `flush_threshold` entries,
// all buffered entries are flushed into LIPP. Lookups check LIPP first (fast path),
// then fall back to the DPGM buffer on miss.
template <class KeyType, class SearchClass, size_t pgm_error, size_t flush_threshold>
class HybridPGMLIPP : public Competitor<KeyType, SearchClass> {
 public:
  HybridPGMLIPP(const std::vector<int>& params) {}

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    buffer_count_ = 0;

    // Bulk load all initial data into LIPP (the read-optimized store)
    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& itm : data) {
      loading_data.push_back(std::make_pair(itm.key, itm.value));
    }

    uint64_t build_time = util::timing([&] {
      lipp_.bulk_load(loading_data.data(), loading_data.size());
      // Initialize an empty DPGM buffer
      std::vector<std::pair<KeyType, uint64_t>> empty;
      pgm_buffer_ = decltype(pgm_buffer_)(empty.begin(), empty.end());
    });

    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    // Try LIPP first (fast path)
    uint64_t value;
    if (lipp_.find(lookup_key, value)) {
      return value;
    }

    // Fall back to DPGM buffer
    if (buffer_count_ > 0) {
      auto it = pgm_buffer_.find(lookup_key);
      if (it != pgm_buffer_.end()) {
        return it->value();
      }
    }

    return util::NOT_FOUND;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key, uint32_t thread_id) const {
    // Sum from LIPP
    uint64_t result = 0;
    auto lit = lipp_.lower_bound(lower_key);
    while (lit != lipp_.end() && lit->comp.data.key <= upper_key) {
      result += lit->comp.data.value;
      ++lit;
    }

    // Sum from DPGM buffer
    if (buffer_count_ > 0) {
      auto pit = pgm_buffer_.lower_bound(lower_key);
      while (pit != pgm_buffer_.end() && pit->key() <= upper_key) {
        result += pit->value();
        ++pit;
      }
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    pgm_buffer_.insert(data.key, data.value);
    buffer_count_++;

    // Flush buffer into LIPP when threshold is reached
    if (buffer_count_ >= flush_threshold) {
      Flush();
    }
  }

  std::string name() const { return "HYBRID"; }

  std::size_t size() const {
    return lipp_.index_size() + pgm_buffer_.size_in_bytes();
  }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    std::string name = SearchClass::name();
    return name != "LinearAVX" && unique && !multithread;
  }

  std::vector<std::string> variants() const {
    std::vector<std::string> vec;
    vec.push_back(SearchClass::name());
    // Encode both pgm_error and flush_threshold as "pgm_error/flush_threshold"
    vec.push_back(std::to_string(pgm_error) + "/" + std::to_string(flush_threshold));
    return vec;
  }

 private:
  void Flush() const {
    // Move all entries from DPGM buffer into LIPP
    auto it = pgm_buffer_.begin();
    while (it != pgm_buffer_.end()) {
      lipp_.insert(it->key(), it->value());
      ++it;
    }

    // Reset buffer
    std::vector<std::pair<KeyType, uint64_t>> empty;
    pgm_buffer_ = decltype(pgm_buffer_)(empty.begin(), empty.end());
    buffer_count_ = 0;
  }

  // LIPP and pgm_buffer_ are mutable because Flush() is called from Insert(),
  // and the benchmark framework passes const references for lookups.
  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                          PGMIndex<KeyType, SearchClass, pgm_error, 16>> pgm_buffer_;
  mutable size_t buffer_count_ = 0;
};
