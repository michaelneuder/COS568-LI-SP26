#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../util.h"
#include "base.h"
#include "pgm_index_dynamic.hpp"
#include "./lipp/src/core/lipp.h"

// Simple bloom filter using double hashing (Murmur3-style mixers).
template <class KeyType>
class SimpleBloomFilter {
 public:
  SimpleBloomFilter(size_t num_bits) : bits_(num_bits, false), num_bits_(num_bits) {}

  void insert(const KeyType& key) {
    uint64_t h1 = hash1(key);
    uint64_t h2 = hash2(key);
    for (int i = 0; i < kNumHashes; i++) {
      bits_[(h1 + i * h2) % num_bits_] = true;
    }
  }

  bool maybe_contains(const KeyType& key) const {
    uint64_t h1 = hash1(key);
    uint64_t h2 = hash2(key);
    for (int i = 0; i < kNumHashes; i++) {
      if (!bits_[(h1 + i * h2) % num_bits_]) return false;
    }
    return true;
  }

  void clear() {
    std::fill(bits_.begin(), bits_.end(), false);
  }

  size_t size_in_bytes() const {
    return (num_bits_ + 7) / 8;
  }

 private:
  static constexpr int kNumHashes = 3;

  static uint64_t hash1(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key;
  }

  static uint64_t hash2(uint64_t key) {
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
  }

  std::vector<bool> bits_;
  size_t num_bits_;
};

// Two-phase hybrid index:
//
//   Phase 1: inserts go into primary_ (DPGM + bloom + side vector for flushing).
//            On reaching flush_threshold, primary_ is drained into LIPP exactly once.
//   Phase 2: inserts go into secondary_ (DPGM + bloom). secondary_ is never flushed.
//
// Lookups check LIPP, then whichever buffer is active for the current phase
// (single bloom check per lookup; no extra cost vs single-buffer design).
template <class KeyType, class SearchClass, size_t pgm_error, size_t flush_threshold>
class HybridPGMLIPP : public Competitor<KeyType, SearchClass> {
 private:
  using PgmType = DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                                  PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  // Cap bloom at 512 KB so it fits in L2 (1 MiB per core on Adroit).
  static constexpr size_t kMaxBloomBits = 512ull * 1024 * 8;  // 512 KB

 public:
  HybridPGMLIPP(const std::vector<int>& params)
      : primary_bloom_(std::min<size_t>(flush_threshold * 10, kMaxBloomBits)),
        secondary_bloom_(std::min<size_t>(flush_threshold * 10, kMaxBloomBits)) {
    // Pre-reserve to avoid reallocations as primary fills toward flush_threshold.
    primary_keys_.reserve(std::min<size_t>(flush_threshold, 2'000'000ull));
  }

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    is_flushed_ = false;
    primary_keys_.clear();
    primary_bloom_.clear();
    secondary_bloom_.clear();

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& itm : data) {
      loading_data.push_back(std::make_pair(itm.key, itm.value));
    }

    uint64_t build_time = util::timing([&] {
      lipp_.bulk_load(loading_data.data(), loading_data.size());
      primary_pgm_ = PgmType();
      secondary_pgm_ = PgmType();
    });

    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    uint64_t value;
    if (lipp_.find(lookup_key, value)) {
      return value;
    }

    if (!is_flushed_) {
      // Phase 1: only primary has data.
      if (!primary_keys_.empty() && primary_bloom_.maybe_contains(lookup_key)) {
        auto it = primary_pgm_.find(lookup_key);
        if (it != primary_pgm_.end()) return it->value();
      }
    } else {
      // Phase 2: primary is drained into LIPP; check secondary.
      if (secondary_bloom_.maybe_contains(lookup_key)) {
        auto it = secondary_pgm_.find(lookup_key);
        if (it != secondary_pgm_.end()) return it->value();
      }
    }

    return util::NOT_FOUND;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key, uint32_t thread_id) const {
    uint64_t result = 0;
    auto lit = lipp_.lower_bound(lower_key);
    while (lit != lipp_.end() && lit->comp.data.key <= upper_key) {
      result += lit->comp.data.value;
      ++lit;
    }

    if (!is_flushed_) {
      if (!primary_keys_.empty()) {
        auto pit = primary_pgm_.lower_bound(lower_key);
        while (pit != primary_pgm_.end() && pit->key() <= upper_key) {
          result += pit->value();
          ++pit;
        }
      }
    } else {
      auto pit = secondary_pgm_.lower_bound(lower_key);
      while (pit != secondary_pgm_.end() && pit->key() <= upper_key) {
        result += pit->value();
        ++pit;
      }
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    if (!is_flushed_) {
      primary_pgm_.insert(data.key, data.value);
      primary_bloom_.insert(data.key);
      primary_keys_.push_back({data.key, data.value});

      if (primary_keys_.size() >= flush_threshold) {
        Flush();
        is_flushed_ = true;
      }
    } else {
      secondary_pgm_.insert(data.key, data.value);
      secondary_bloom_.insert(data.key);
    }
  }

  std::string name() const { return "HYBRID"; }

  std::size_t size() const {
    return lipp_.index_size()
           + primary_pgm_.size_in_bytes() + primary_bloom_.size_in_bytes()
           + secondary_pgm_.size_in_bytes() + secondary_bloom_.size_in_bytes();
  }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    std::string name = SearchClass::name();
    return name != "LinearAVX" && unique && !multithread;
  }

  std::vector<std::string> variants() const {
    std::vector<std::string> vec;
    vec.push_back(SearchClass::name());
    vec.push_back(std::to_string(pgm_error) + "/" + std::to_string(flush_threshold));
    return vec;
  }

 private:
  void Flush() const {
    for (const auto& kv : primary_keys_) {
      lipp_.insert(kv.first, kv.second);
    }
    primary_keys_.clear();
    primary_pgm_ = PgmType();
    primary_bloom_.clear();
  }

  // Mutable because Flush() is called from Insert() and the benchmark
  // framework may pass const references for lookups.
  mutable LIPP<KeyType, uint64_t> lipp_;

  // Primary buffer (Phase 1): receives all inserts up to flush_threshold,
  // then drained to LIPP exactly once.
  mutable PgmType primary_pgm_;
  mutable SimpleBloomFilter<KeyType> primary_bloom_;
  mutable std::vector<std::pair<KeyType, uint64_t>> primary_keys_;

  // Secondary buffer (Phase 2): receives all inserts after the one flush.
  // Never flushed — accumulates until destruction.
  mutable PgmType secondary_pgm_;
  mutable SimpleBloomFilter<KeyType> secondary_bloom_;

  mutable bool is_flushed_ = false;
};
