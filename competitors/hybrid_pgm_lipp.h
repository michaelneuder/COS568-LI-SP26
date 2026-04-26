#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../util.h"
#include "base.h"
#include "pgm_index_dynamic.hpp"
#include "./lipp/src/core/lipp.h"

// Simple counting bloom filter (so we can clear it on flush by resetting the bit array).
// Uses k=3 hash functions derived from two base hashes via double hashing.
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

  // Two simple hash functions based on multiplicative hashing.
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

// Hybrid index: uses DynamicPGM as a write buffer and LIPP as the read-optimized store.
// A bloom filter sits in front of the DPGM buffer to short-circuit lookups for keys
// that are definitely not in the buffer (avoiding the DPGM lookup cost).
//
// - On insert: key goes into the DPGM buffer + bloom filter.
// - On lookup: check LIPP first. If miss, check bloom filter — if "not in buffer",
//              return NOT_FOUND immediately. Otherwise check DPGM.
// - On flush: bulk-insert all buffered entries into LIPP, reset buffer + bloom filter.
template <class KeyType, class SearchClass, size_t pgm_error, size_t flush_threshold>
class HybridPGMLIPP : public Competitor<KeyType, SearchClass> {
 public:
  // Bloom filter sized at ~10 bits per expected entry to keep false positive rate low.
  HybridPGMLIPP(const std::vector<int>& params)
      : bloom_(flush_threshold * 10) {}

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    flush_keys_.clear();
    bloom_.clear();

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& itm : data) {
      loading_data.push_back(std::make_pair(itm.key, itm.value));
    }

    uint64_t build_time = util::timing([&] {
      lipp_.bulk_load(loading_data.data(), loading_data.size());
      pgm_buffer_ = decltype(pgm_buffer_)();
    });

    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    // Try LIPP first (fast path)
    uint64_t value;
    if (lipp_.find(lookup_key, value)) {
      return value;
    }

    // Bloom filter short-circuits the DPGM lookup if the key is definitely not in the buffer.
    if (!flush_keys_.empty() && bloom_.maybe_contains(lookup_key)) {
      auto it = pgm_buffer_.find(lookup_key);
      if (it != pgm_buffer_.end()) {
        return it->value();
      }
    }

    return util::NOT_FOUND;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key, uint32_t thread_id) const {
    // Bloom filter doesn't help with ranges — always scan both indexes.
    uint64_t result = 0;
    auto lit = lipp_.lower_bound(lower_key);
    while (lit != lipp_.end() && lit->comp.data.key <= upper_key) {
      result += lit->comp.data.value;
      ++lit;
    }

    if (!flush_keys_.empty()) {
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
    flush_keys_.push_back({data.key, data.value});
    bloom_.insert(data.key);

    if (flush_keys_.size() >= flush_threshold) {
      Flush();
    }
  }

  std::string name() const { return "HYBRID"; }

  std::size_t size() const {
    return lipp_.index_size() + pgm_buffer_.size_in_bytes() + bloom_.size_in_bytes();
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
    for (const auto& kv : flush_keys_) {
      lipp_.insert(kv.first, kv.second);
    }
    flush_keys_.clear();
    pgm_buffer_ = decltype(pgm_buffer_)();
    bloom_.clear();
  }

  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                          PGMIndex<KeyType, SearchClass, pgm_error, 16>> pgm_buffer_;
  mutable std::vector<std::pair<KeyType, uint64_t>> flush_keys_;
  mutable SimpleBloomFilter<KeyType> bloom_;
};