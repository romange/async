// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/fiber_sched_algo.h"

namespace util {
namespace epoll {

class EpollFiberAlgo : public FiberSchedAlgo {

 public:
   explicit EpollFiberAlgo(ProactorBase* proactor);
  ~EpollFiberAlgo();

  // suspend_until halts the thread in case there are no active fibers to run on it.
  // This is done by dispatcher fiber.
  void suspend_until(time_point const& abs_time) noexcept final;
  //]

 private:
  unsigned arm_index_;
};

}  // namespace uring
}  // namespace util
