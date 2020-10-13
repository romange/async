// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/fiber_sched_algo.h"

#include <sys/poll.h>
#include <sys/timerfd.h>

#include "base/logging.h"
#include "util/uring/proactor.h"

// TODO: We should replace DVLOG macros with RAW_VLOG if we do glog sync integration.

namespace util {
using namespace boost;
using namespace std;

FiberSchedAlgo::FiberSchedAlgo(ProactorBase* proactor) : proactor_(proactor) {
  main_cntx_ = fibers::context::active();
  CHECK(main_cntx_->is_context(fibers::type::main_context));
}

FiberSchedAlgo::~FiberSchedAlgo() {
}

void FiberSchedAlgo::awakened(FiberContext* ctx, FiberProps& props) noexcept {
  DCHECK(!ctx->ready_is_linked());

  if (ctx->is_context(fibers::type::dispatcher_context)) {
    DVLOG(2) << "Awakened dispatch";
  } else {
    DVLOG(2) << "Awakened " << props.name();

    ++ready_cnt_;  // increase the number of awakened/ready fibers.
  }

  ctx->ready_link(rqueue_); /*< fiber, enqueue on ready queue >*/
}

auto FiberSchedAlgo::pick_next() noexcept -> FiberContext* {
  DVLOG(2) << "pick_next: " << ready_cnt_ << "/" << rqueue_.size();

  if (rqueue_.empty())
    return nullptr;

  FiberContext* ctx = &rqueue_.front();
  rqueue_.pop_front();

  if (!ctx->is_context(boost::fibers::type::dispatcher_context)) {
    --ready_cnt_;
    FiberProps* props = (FiberProps*)ctx->get_properties();
    DVLOG(1) << "Switching to " << props->name();  // TODO: to switch to RAW_LOG.
  } else {
    DVLOG(1) << "Switching to dispatch";  // TODO: to switch to RAW_LOG.
  }
  return ctx;
}

void FiberSchedAlgo::property_change(FiberContext* ctx, FiberProps& props) noexcept {
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

bool FiberSchedAlgo::has_ready_fibers() const noexcept {
  return ready_cnt_ > 0;
}

// This function is called from remote threads, to wake this thread in case it's sleeping.
// In our case, "sleeping" means - might stuck the wait function waiting for completion events.
// wait_for_cqe is the only place where the thread can be stalled.
void FiberSchedAlgo::notify() noexcept {
  DVLOG(1) << "notify from " << syscall(SYS_gettid);

  // We signal so that
  // 1. Main context should awake if it is not
  // 2. it needs to yield to dispatch context that will put active fibers into
  // ready queue.
  auto prev_val = proactor_->tq_seq_.fetch_or(1, std::memory_order_relaxed);
  if (prev_val == ProactorBase::WAIT_SECTION_STATE) {
    ProactorBase* from = ProactorBase::me();
    if (from)
      from->algo_notify_cnt_.fetch_add(1, std::memory_order_relaxed);
    proactor_->WakeRing();
  }
}

}  // namespace util
