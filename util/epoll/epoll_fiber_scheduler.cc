// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/epoll/epoll_fiber_scheduler.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "base/logging.h"
#include "util/epoll/ev_controller.h"

namespace util {
namespace epoll {
using namespace boost;
using namespace std;

static void WakeMyFiber(uint32_t event_mask, EvController*) {
  DVLOG(1) << "this_fiber::yield " << event_mask;

  this_fiber::yield();
}

EpollFiberAlgo::EpollFiberAlgo(EvController* ev_cntr) : ev_cntrl_(ev_cntr) {
  main_cntx_ = fibers::context::active();
  CHECK(main_cntx_->is_context(fibers::type::main_context));

  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  CHECK_GE(timer_fd_, 0);

  arm_index_ = ev_cntrl_->Arm(timer_fd_, &WakeMyFiber, EPOLLIN);
}

EpollFiberAlgo::~EpollFiberAlgo() {
  ev_cntrl_->Disarm(arm_index_);

  close(timer_fd_);
}

void EpollFiberAlgo::awakened(FiberContext* ctx, EpollFiberProps& props) noexcept {
  DCHECK(!ctx->ready_is_linked());

  if (ctx->is_context(fibers::type::dispatcher_context)) {
    DVLOG(2) << "Awakened dispatch";
  } else {
    DVLOG(2) << "Awakened " << props.name();

    ++ready_cnt_;  // increase the number of awakened/ready fibers.
  }

  ctx->ready_link(rqueue_); /*< fiber, enqueue on ready queue >*/
}

auto EpollFiberAlgo::pick_next() noexcept -> FiberContext* {
  DVLOG(2) << "pick_next: " << ready_cnt_ << "/" << rqueue_.size();

  if (rqueue_.empty())
    return nullptr;

  FiberContext* ctx = &rqueue_.front();
  rqueue_.pop_front();

  if (!ctx->is_context(boost::fibers::type::dispatcher_context)) {
    --ready_cnt_;
    EpollFiberProps* props = (EpollFiberProps*)ctx->get_properties();
    DVLOG(1) << "Switching to " << props->name();  // TODO: to switch to RAW_LOG.
  } else {
    DVLOG(1) << "Switching to dispatch";  // TODO: to switch to RAW_LOG.
  }
  return ctx;
}

void EpollFiberAlgo::property_change(FiberContext* ctx, EpollFiberProps& props) noexcept {
  if (!ctx->ready_is_linked()) {
    return;
  }

  // Found ctx: unlink it
  ctx->ready_unlink();
  if (!ctx->is_context(fibers::type::dispatcher_context)) {
    --ready_cnt_;
  }

  // Here we know that ctx was in our ready queue, but we've unlinked
  // it. We happen to have a method that will (re-)add a context* to the
  // right place in the ready queue.
  awakened(ctx, props);
}

bool EpollFiberAlgo::has_ready_fibers() const noexcept {
  return ready_cnt_ > 0;
}

// suspend_until halts the thread in case there are no active fibers to run on it.
// This function is called by dispatcher fiber.
void EpollFiberAlgo::suspend_until(const time_point& abs_time) noexcept {
  auto* cur_cntx = fibers::context::active();

  DCHECK(cur_cntx->is_context(fibers::type::dispatcher_context));
  if (time_point::max() != abs_time) {
    using namespace chrono;
    constexpr uint64_t kNsFreq = 1000000000ULL;
    const chrono::time_point<steady_clock, nanoseconds>& tp = abs_time;
    int64_t ns = time_point_cast<nanoseconds>(tp).time_since_epoch().count();

    struct itimerspec abs_spec;

    abs_spec.it_value.tv_sec = ns / kNsFreq;
    abs_spec.it_value.tv_nsec = ns - abs_spec.it_value.tv_sec * kNsFreq;

    memset(&abs_spec.it_interval, 0, sizeof(abs_spec.it_interval));

    int res = timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &abs_spec, NULL);
    CHECK_EQ(0, res) << strerror(errno);
  }

  // schedule does not block just marks main_cntx_ for activation.
  main_cntx_->get_scheduler()->schedule(main_cntx_);
}

// This function is called from remote threads, to wake this thread in case it's sleeping.
// In our case, "sleeping" means - might stuck the wait function waiting for completion events.
// wait_for_cqe is the only place where the thread can be stalled.
void EpollFiberAlgo::notify() noexcept {
  DVLOG(1) << "notify from " << syscall(SYS_gettid);

  // We signal so that
  // 1. Main context should awake if it is not
  // 2. it needs to yield to dispatch context that will put active fibers into
  // ready queue.
  auto prev_val = ev_cntrl_->tq_seq_.fetch_or(1, std::memory_order_relaxed);
  if (prev_val == EvController::WAIT_SECTION_STATE) {
    ev_cntrl_->DoWake();
  }
}

}  // namespace epoll
}  // namespace util
