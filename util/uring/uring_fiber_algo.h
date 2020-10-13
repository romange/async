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

  // suspend_until halts the thread in case there are no active fibers to run on it.
  // This is done by dispatcher fiber.
  void suspend_until(time_point const& abs_time) noexcept final;
  //]

 private:
  timespec ts_;
  int timer_fd_ = -1;
};

}  // namespace uring
}  // namespace util
