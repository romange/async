// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include <boost/fiber/scheduler.hpp>
#include <string>

namespace util {
namespace epoll {

class EpollFiberProps : public ::boost::fibers::fiber_properties {
 public:
  EpollFiberProps(::boost::fibers::context* ctx) : fiber_properties(ctx) {
  }

  void set_name(std::string nm) {
    name_ = std::move(nm);
  }

  const std::string& name() const {
    return name_;
  }

 private:
  std::string name_;
};

class EvController;

class EpollFiberAlgo : public ::boost::fibers::algo::algorithm_with_properties<EpollFiberProps> {
  using ready_queue_type = ::boost::fibers::scheduler::ready_queue_type;

 public:
  using FiberContext = ::boost::fibers::context;
  using time_point = std::chrono::steady_clock::time_point;

  explicit EpollFiberAlgo(EvController* proactor);
  ~EpollFiberAlgo();

  void awakened(FiberContext* ctx, EpollFiberProps& props) noexcept override;

  FiberContext* pick_next() noexcept override;

  void property_change(FiberContext* ctx, EpollFiberProps& props) noexcept final;

  bool has_ready_fibers() const noexcept final;

  // suspend_until halts the thread in case there are no active fibers to run on it.
  // This is done by dispatcher fiber.
  void suspend_until(time_point const& abs_time) noexcept final;
  //]

  // This function is called from remote threads, to wake this thread in case it's sleeping.
  // In our case, "sleeping" means - might stuck the wait function waiting for completion events.
  void notify() noexcept final;

 private:
  ready_queue_type rqueue_;
  EvController* ev_cntrl_;
  FiberContext* main_cntx_;
  uint32_t ready_cnt_ = 0;
  int timer_fd_ = -1;
  unsigned arm_index_;
};

}  // namespace uring
}  // namespace util
