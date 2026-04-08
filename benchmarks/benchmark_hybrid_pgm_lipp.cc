#include "benchmarks/benchmark_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "common.h"
#include "competitors/hybrid_pgm_lipp.h"

template <typename Searcher>
void benchmark_64_hybrid(tli::Benchmark<uint64_t>& benchmark,
                         bool pareto, const std::vector<int>& params) {
  // Sweep over different flush thresholds and PGM error bounds
  if (!pareto) {
    util::fail("HYBRID's hyperparameters cannot be set without pareto mode");
  } else {
    // Vary flush_threshold: 1000, 10000, 100000
    // Vary pgm_error: 64, 128, 256, 512
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 64, 1000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 64, 10000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 64, 100000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 128, 1000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 128, 10000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 128, 100000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 256, 1000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 256, 10000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 256, 100000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 512, 1000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 512, 10000>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, Searcher, 512, 100000>>();
  }
}

template <int record>
void benchmark_64_hybrid(tli::Benchmark<uint64_t>& benchmark,
                         const std::string& filename) {
  // Default configurations — a reasonable subset to start with.
  // You can refine these after seeing initial results.
  benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 1000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 10000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 100000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, LinearSearch<record>, 128, 1000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, LinearSearch<record>, 128, 10000>>();
  benchmark.template Run<HybridPGMLIPP<uint64_t, LinearSearch<record>, 128, 100000>>();
}

INSTANTIATE_TEMPLATES_MULTITHREAD(benchmark_64_hybrid, uint64_t);