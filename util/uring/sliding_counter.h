// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <numeric>

#include "util/uring/proactor_pool.h"

namespace util {
namespace uring {

namespace detail {

class SlidingCounterTLBase {
 protected:
  // Returns the bin corresponding to the current timestamp. Has second precision.
  // updates last_ts_ according to the current timestamp and returns the latest bin.
  // has const semantics even though it updates mutable last_ts_.
  uint32_t MoveTsIfNeeded(size_t size, int32_t* dest) const;

  mutable uint32_t last_ts_ = 0;
};

class SlidingCounterBase {
 protected:
  void InitInternal(ProactorPool* pp);
  void CheckInit() const;
  unsigned ProactorThreadIndex() const;

  ProactorPool* pp_ = nullptr;
};

}  // namespace detail

/**
 * @brief Sliding window data structure that can aggregate moving statistics.
 *        It's implmented using ring-buffer with size specified at compile time.
 *
 * @tparam NUM
 */
template <unsigned NUM> class SlidingCounterTL : protected detail::SlidingCounterTLBase {
  static_assert(NUM > 1, "Invalid window size");

  using T = int32_t;
  mutable std::array<T, NUM> count_;

 public:
  SlidingCounterTL() {
    Reset();
  }

  void Inc() {
    IncBy(1);
  }

  void IncBy(int32_t delta) {
    int32_t bin = MoveTsIfNeeded(NUM, count_.data());
    count_[bin] += delta;
  }

  // Sums over bins not including the last bin that is currently being filled.
  T SumTail() const;

  T Sum() const {
    MoveTsIfNeeded(NUM, count_.data());
    return std::accumulate(count_.begin(), count_.end(), 0);
  }

  void Reset() {
    count_.fill(0);
  }
};

// Requires proactor_pool initialize all the proactors.
template <unsigned NUM> class SlidingCounter : protected detail::SlidingCounterBase {
  using Counter = SlidingCounterTL<NUM>;

 public:
  enum {WIN_SIZE = NUM};

  SlidingCounter() = default;

  void Init(ProactorPool* pp) {
    InitInternal(pp);
    sc_thread_map_.reset(new Counter[pp_->size()]);
  }

  void Inc() {
    sc_thread_map_[ProactorThreadIndex()].Inc();
  }

  uint32_t Sum() const {
    CheckInit();

    std::atomic_uint32_t res{0};
    pp_->AwaitOnAll([&](unsigned i, auto*) {
      res.fetch_add(sc_thread_map_[i].Sum(), std::memory_order_relaxed);
    });

    return res.load(std::memory_order_release);
  }

  uint32_t SumTail() const {
    CheckInit();

    std::atomic_uint32_t res{0};
    pp_->AwaitOnAll([&](unsigned i, auto*) {
      res.fetch_add(sc_thread_map_[i].SumTail(), std::memory_order_relaxed);
    });

    return res.load(std::memory_order_release);
  }

 private:
  std::unique_ptr<Counter[]> sc_thread_map_;
};

/*********************************************
 Implementation section.
**********************************************/

template <unsigned NUM> auto SlidingCounterTL<NUM>::SumTail() const -> T {
  int32_t start = MoveTsIfNeeded(NUM, count_.data()) + 1;  // the tail is one after head.

  T sum = 0;
  for (unsigned i = 0; i < NUM - 1; ++i) {
    sum += count_[(start + i) % NUM];
  }
  return sum;
}

}  // namespace uring
}  // namespace util
