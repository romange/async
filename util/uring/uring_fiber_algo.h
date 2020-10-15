// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/fiber_sched_algo.h"

namespace util {
namespace uring {
class Proactor;

using UringFiberProps  = FiberProps;

class UringFiberAlgo : public FiberSchedAlgo {

 public:
  explicit UringFiberAlgo(Proactor* proactor);
  ~UringFiberAlgo();

 private:
  void SuspendWithTimer(const time_point& tp) noexcept final;

  timespec ts_;
  int timer_fd_ = -1;
};

}  // namespace uring
}  // namespace util
