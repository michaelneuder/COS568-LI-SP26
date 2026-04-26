#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
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

// Async double-buffered hybrid index: DPGM is the write buffer, LIPP is the
// read-optimized store. When the active buffer fills, swap it with a flushing
// buffer and signal a background thread to drain into LIPP. The main thread
// keeps inserting into the new (empty) active buffer without blocking.
//
// Lookups check LIPP, then both buffers (with bloom filters as short-circuits).
template <class KeyType, class SearchClass, size_t pgm_error, size_t flush_threshold>
class HybridPGMLIPP : public Competitor<KeyType, SearchClass> {
 private:
  using PgmType = DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                                  PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  struct BufferState {
    PgmType pgm;
    SimpleBloomFilter<KeyType> bloom;
    std::vector<std::pair<KeyType, uint64_t>> keys;

    BufferState() : bloom(flush_threshold * 10) {}

    void clear() {
      pgm = PgmType();
      bloom.clear();
      keys.clear();
    }

    bool empty() const { return keys.empty(); }
    size_t size() const { return keys.size(); }
  };

 public:
  HybridPGMLIPP(const std::vector<int>& params)
      : active_(std::make_unique<BufferState>()),
        flushing_(std::make_unique<BufferState>()) {
    start_worker();
  }

  ~HybridPGMLIPP() {
    stop_worker();
  }

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    // Worker should be idle here (Build called once at start of each repeat).
    // Reset state defensively.
    {
      std::unique_lock<std::mutex> lk(swap_mutex_);
      flush_pending_.store(false, std::memory_order_release);
      active_->clear();
      flushing_->clear();
    }

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& itm : data) {
      loading_data.push_back(std::make_pair(itm.key, itm.value));
    }

    uint64_t build_time = util::timing([&] {
      lipp_.bulk_load(loading_data.data(), loading_data.size());
    });

    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    // Fast path: LIPP. Shared lock — multiple lookups can proceed in parallel,
    // but the background flush takes an exclusive lock and blocks lookups.
    {
      std::shared_lock<std::shared_mutex> lk(lipp_mutex_);
      uint64_t value;
      if (lipp_.find(lookup_key, value)) {
        return value;
      }
    }

    // Check active buffer (no lock — only mutated by main thread in single-
    // threaded benchmark; bloom filter is cheap and short-circuits negatives).
    if (!active_->empty() && active_->bloom.maybe_contains(lookup_key)) {
      auto it = active_->pgm.find(lookup_key);
      if (it != active_->pgm.end()) return it->value();
    }

    // Check flushing buffer if a flush is in progress (worker is reading it
    // but not mutating; safe to read concurrently).
    if (flush_pending_.load(std::memory_order_acquire) &&
        !flushing_->empty() && flushing_->bloom.maybe_contains(lookup_key)) {
      auto it = flushing_->pgm.find(lookup_key);
      if (it != flushing_->pgm.end()) return it->value();
    }

    return util::NOT_FOUND;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key, uint32_t thread_id) const {
    uint64_t result = 0;
    {
      std::shared_lock<std::shared_mutex> lk(lipp_mutex_);
      auto lit = lipp_.lower_bound(lower_key);
      while (lit != lipp_.end() && lit->comp.data.key <= upper_key) {
        result += lit->comp.data.value;
        ++lit;
      }
    }

    if (!active_->empty()) {
      auto pit = active_->pgm.lower_bound(lower_key);
      while (pit != active_->pgm.end() && pit->key() <= upper_key) {
        result += pit->value();
        ++pit;
      }
    }

    if (flush_pending_.load(std::memory_order_acquire) && !flushing_->empty()) {
      auto pit = flushing_->pgm.lower_bound(lower_key);
      while (pit != flushing_->pgm.end() && pit->key() <= upper_key) {
        result += pit->value();
        ++pit;
      }
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    active_->pgm.insert(data.key, data.value);
    active_->bloom.insert(data.key);
    active_->keys.push_back({data.key, data.value});

    if (active_->size() >= flush_threshold) {
      // Try to swap. If the previous flush hasn't finished, just keep
      // accumulating in active_ (back-pressure).
      std::unique_lock<std::mutex> lk(swap_mutex_);
      if (!flush_pending_.load(std::memory_order_acquire)) {
        // Worker is idle; flushing_ holds stale data from the previous flush.
        // Clear it now (under swap_mutex_) before reusing it as the new active_.
        // This is safe because flush_pending_ is false, so no lookup will
        // touch flushing_ concurrently.
        flushing_->clear();
        std::swap(active_, flushing_);
        flush_pending_.store(true, std::memory_order_release);
        flush_cv_.notify_one();
      }
    }
  }

  std::string name() const { return "HYBRID"; }

  std::size_t size() const {
    return lipp_.index_size() + active_->pgm.size_in_bytes()
           + flushing_->pgm.size_in_bytes()
           + active_->bloom.size_in_bytes()
           + flushing_->bloom.size_in_bytes();
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
  void start_worker() {
    stop_.store(false, std::memory_order_release);
    flush_pending_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { worker_loop(); });
  }

  void stop_worker() {
    {
      std::lock_guard<std::mutex> lk(swap_mutex_);
      stop_.store(true, std::memory_order_release);
      flush_cv_.notify_one();
    }
    if (worker_.joinable()) worker_.join();
  }

  void worker_loop() {
    while (true) {
      std::unique_lock<std::mutex> lk(swap_mutex_);
      flush_cv_.wait(lk, [&] {
        return stop_.load(std::memory_order_acquire) ||
               flush_pending_.load(std::memory_order_acquire);
      });
      if (stop_.load(std::memory_order_acquire)) return;
      lk.unlock();

      // Drain flushing_ into LIPP. Take exclusive lock per batch to allow
      // lookups to interleave between batches. Read-only access to flushing_;
      // we never clear it here — that's the next swap's job (under swap_mutex_)
      // to avoid use-after-clear races with concurrent lookups.
      constexpr size_t kBatchSize = 256;
      size_t i = 0;
      const size_t n = flushing_->keys.size();
      while (i < n) {
        size_t end = std::min(i + kBatchSize, n);
        {
          std::unique_lock<std::shared_mutex> lipp_lk(lipp_mutex_);
          for (; i < end; ++i) {
            lipp_.insert(flushing_->keys[i].first, flushing_->keys[i].second);
          }
        }
      }

      flush_pending_.store(false, std::memory_order_release);
    }
  }

  // LIPP store + reader/writer lock.
  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable std::shared_mutex lipp_mutex_;

  // Double buffer. Pointers are swapped under swap_mutex_.
  std::unique_ptr<BufferState> active_;
  std::unique_ptr<BufferState> flushing_;
  std::mutex swap_mutex_;

  // Background worker.
  std::thread worker_;
  std::condition_variable flush_cv_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> flush_pending_{false};
};