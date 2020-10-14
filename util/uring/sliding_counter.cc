// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/uring/sliding_counter.h"

#include "absl/time/clock.h"
#include "base/logging.h"

using namespace std;

namespace util {
namespace uring {
namespace detail {

uint32_t SlidingCounterTLBase::MoveTsIfNeeded(size_t size, int32_t* dest) const {
  uint32_t current_sec = time(NULL);
  if (last_ts_ + size <= current_sec) {
    std::fill(dest, dest + size, 0);
  } else {
    // Reset delta upto current_time including.
    for (uint32_t i = last_ts_ + 1; i <= current_sec; ++i) {
      dest[i % size] = 0;
    }
  }
  last_ts_ = current_sec;

  return current_sec % size;
}

void SlidingCounterBase::InitInternal(ProactorPool* pp) {
  CHECK(pp_ == nullptr);
  pp_ = CHECK_NOTNULL(pp);
}

void SlidingCounterBase::CheckInit() const {
  CHECK_NOTNULL(pp_);
}

unsigned SlidingCounterBase::ProactorThreadIndex() const {
  int32_t tnum = CHECK_NOTNULL(pp_)->size();

  int32_t indx = ProactorBase::GetIndex();
  CHECK_GE(indx, 0) << "Must be called from proactor thread!";
  CHECK_LT(indx, tnum) << "Invalid thread index " << indx;

  return unsigned(indx);
}

}  // namespace detail
}  // namespace uring
}  // namespace util
