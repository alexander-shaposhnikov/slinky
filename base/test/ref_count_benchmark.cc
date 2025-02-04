#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>
#include <vector>

#include <iostream>

#include "base/ref_count.h"

namespace slinky {

struct TestObject: ref_counted<TestObject> {
  static void destroy(TestObject *obj) {}
};

static void BM_RefCount_AddRelease(benchmark::State &state) {
  const int num_threads = state.threads();

  //std::cerr << "num_threads = " << num_threads << std::endl;

  // Equal to the number of add/release pairs per thread.
  const int iterations = state.range(0);

  //std::cerr << "iterations = " << iterations << std::endl;

  for (auto _ : state) {
    ref_counted<TestObject> rc;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; ++i) {
      threads.emplace_back([&rc, iterations]() {
        for (int j = 0; j < iterations; ++j) {
          rc.add_ref();
          rc.release();
        }
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    //std::cerr << "rc.ref_count() = " << rc.ref_count() << std::endl;
  }
}

BENCHMARK(BM_RefCount_AddRelease)
    ->RangeMultiplier(32)
    ->Ranges({{16, 1024}})
    ->ThreadRange(std::thread::hardware_concurrency(), std::thread::hardware_concurrency()); //std::thread::hardware_concurrency());

} // namespace slinky
