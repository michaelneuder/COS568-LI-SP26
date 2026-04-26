#include "benchmarks/benchmark_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "common.h"
#include "competitors/hybrid_pgm_lipp.h"

template <typename Searcher>
void benchmark_64_hybrid(tli::Benchmark<uint64_t>& benchmark,
                         bool pareto, const std::vector<int>& params) {
  if (!pareto) {
    util::fail("HYBRID's hyperparameters cannot be set without pareto mode");
  } else {
    // Sweep pgm_error in {64, 128, 256} x flush_threshold in {10000, 100000, 1000000}
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 64, 10000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 64, 100000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 64, 1000000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 128, 10000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 128, 100000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 128, 1000000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 256, 10000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 256, 100000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 256, 1000000>>();
  }
}

template <int record>
void benchmark_64_hybrid(tli::Benchmark<uint64_t>& benchmark,
                         const std::string& filename) {
  // Default sweep: 2 search methods x 3 flush thresholds x 1 pgm_error.
  // Threshold range covers "frequent flush" (10000), "moderate" (100000),
  // and "never flush in 200K-insert workload" (1000000).
  benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 10000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 100000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 1000000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, LinearSearch<record>, 128, 10000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, LinearSearch<record>, 128, 100000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, LinearSearch<record>, 128, 1000000>>();
}

INSTANTIATE_TEMPLATES_MULTITHREAD(benchmark_64_hybrid, uint64_t);
