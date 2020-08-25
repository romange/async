// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/uring_fiber_algo.h"

#include "base/logging.h"
#include "util/uring/proactor.h"

// TODO: We should replace DVLOG macros with RAW_VLOG if we do glog sync integration.

namespace util {
namespace uring {
using namespace boost;
using namespace std;

UringFiberAlgo::UringFiberAlgo(Proactor* proactor) : proactor_(proactor) {
  main_cntx_ = fibers::context::active();
  CHECK(main_cntx_->is_context(fibers::type::main_context));
}

UringFiberAlgo::~UringFiberAlgo() {
}

void UringFiberAlgo::awakened(FiberContext* ctx, UringFiberProps& props) noexcept {
  DCHECK(!ctx->ready_is_linked());

  if (ctx->is_context(fibers::type::dispatcher_context)) {
    DVLOG(2) << "Awakened dispatch";
  } else {
    DVLOG(2) << "Awakened " << props.name();

    ++ready_cnt_;  // increase the number of awakened/ready fibers.
  }

  ctx->ready_link(rqueue_); /*< fiber, enqueue on ready queue >*/
}

auto UringFiberAlgo::pick_next() noexcept -> FiberContext* {
  DVLOG(2) << "pick_next: " << ready_cnt_ << "/" << rqueue_.size();

  if (rqueue_.empty())
    return nullptr;

  FiberContext* ctx = &rqueue_.front();
  rqueue_.pop_front();

  if (!ctx->is_context(boost::fibers::type::dispatcher_context)) {
    --ready_cnt_;
    UringFiberProps* props = (UringFiberProps*)ctx->get_properties();
    DVLOG(1) << "Switching to " << props->name();  // TODO: to switch to RAW_LOG.
  } else {
    DVLOG(1) << "Switching to dispatch";  // TODO: to switch to RAW_LOG.
  }
  return ctx;
}

void UringFiberAlgo::property_change(FiberContext* ctx, UringFiberProps& props) noexcept {
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

bool UringFiberAlgo::has_ready_fibers() const noexcept {
  return ready_cnt_ > 0;
}

// suspend_until halts the thread in case there are no active fibers to run on it.
// This function is called by dispatcher fiber.
void UringFiberAlgo::suspend_until(const time_point& abs_time) noexcept {
  auto* cur_cntx = fibers::context::active();

  DCHECK(cur_cntx->is_context(fibers::type::dispatcher_context));
  if (time_point::max() != abs_time) {
    auto cb = [](Proactor::IoResult res, int64_t, Proactor*) {
      // If io_uring does not support timeout, then this callback will be called
      // earlier than needed and dispatch won't awake the sleeping fiber.
      // This will cause deadlock.
      DCHECK_NE(res, -EINVAL) << "This linux version does not support this operation";
      DVLOG(1) << "this_fiber::yield " << res;

      this_fiber::yield();
    };

    // TODO: if we got here, most likely our completion queues were empty so
    // it's unlikely that we will have full submit queue but this state may happen.
    // GetSubmitEntry may block which may cause a deadlock since our main loop is not
    // running (it's probably in suspend mode letting dispatcher fiber to run).
    // Therefore we must use here non blocking calls.
    // But what happens if SQ is full?
    // SQ is full we can not use IoUring to schedule awake event, our CQ queue is empty so
    // we have nothing to process. We might want to give up on this timer and just wait on CQ
    // since we know something might come up. On the other hand, imagine we send requests on sockets
    // but they all do not answer so SQ is eventually full, CQ is empty and our IO loop is overflown
    // and no entries could be processed.
    // We must reproduce this case: small SQ/CQ. Fill SQ/CQ with alarms that expire in a long time.
    // So at some point SQ-push returns EBUSY. Now we call this_fiber::sleep and we GetSubmitEntry
    // would block.
    SubmitEntry se = proactor_->GetSubmitEntry(std::move(cb), 0);
    using namespace chrono;
    constexpr uint64_t kNsFreq = 1000000000ULL;
    int64_t ns;
    const chrono::time_point<steady_clock, nanoseconds>& tp = abs_time;

    // 5.4 does not support absolute timespecs.
    bool pass_abs = proactor_->support_abs_timeout_;
    if (pass_abs) {
      ns = time_point_cast<nanoseconds>(tp).time_since_epoch().count();
    } else {
      ns = chrono::nanoseconds(tp - steady_clock::now()).count();
      if (ns < 0) ns = 0;
    }
    ts_.tv_sec = ns / kNsFreq;
    ts_.tv_nsec = ns - ts_.tv_sec * kNsFreq;

    // Please note that we can not pass var on stack because we exit from the function
    // before we submit to ring. That's why ts_ is a data member.
    se.PrepTimeout(&ts_, pass_abs);
  }

  // schedule does not block just marks main_cntx_ for activation.
  main_cntx_->get_scheduler()->schedule(main_cntx_);
}

// This function is called from remote threads, to wake this thread in case it's sleeping.
// In our case, "sleeping" means - might stuck the wait function waiting for completion events.
// wait_for_cqe is the only place where the thread can be stalled.
void UringFiberAlgo::notify() noexcept {
  DVLOG(1) << "notify from " << syscall(SYS_gettid);

  // We signal so that
  // 1. Main context should awake if it is not
  // 2. it needs to yield to dispatch context that will put active fibers into
  // ready queue.
  auto prev_val = proactor_->tq_seq_.fetch_or(1, std::memory_order_relaxed);
  if (prev_val == Proactor::WAIT_SECTION_STATE) {
    proactor_->WakeRing();
  }
}

}  // namespace uring
}  // namespace util
