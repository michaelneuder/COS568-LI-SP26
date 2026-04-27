#! /usr/bin/env bash

echo "Executing HYBRID, DynamicPGM, and LIPP benchmarks on mixed workloads..."

BENCHMARK=build/benchmark
if [ ! -f $BENCHMARK ]; then
    echo "benchmark binary does not exist"
    exit
fi

function execute_uint64_100M() {
    echo "Executing operations for $1 and index $2"
    echo "Executing insert+lookup mixed workload with insert-ratio 0.9"
    $BENCHMARK ./data/$1 ./data/$1_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix --through --csv --only $2 -r 3
    echo "Executing insert+lookup mixed workload with insert-ratio 0.1"
    $BENCHMARK ./data/$1 ./data/$1_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix --through --csv --only $2 -r 3
}

mkdir -p ./results2

for DATA in fb_100M_public_uint64 books_100M_public_uint64 osmc_100M_public_uint64
do
for INDEX in HYBRID DynamicPGM LIPP
do
    execute_uint64_100M ${DATA} $INDEX
done
done

echo "===================Benchmarking complete!===================="