// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/epoll/ev_controller.h"

#include <string.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <boost/fiber/operations.hpp>
#include <boost/fiber/scheduler.hpp>

#include "absl/base/attributes.h"
#include "absl/time/clock.h"
#include "base/logging.h"
#include "base/proc_util.h"
#include "util/epoll/epoll_fiber_scheduler.h"

#define EV_CHECK(x)                                                           \
  do {                                                                           \
    int __res_val = (x);                                                         \
    if (ABSL_PREDICT_FALSE(__res_val < 0)) {                                     \
      char buf[128];                                                             \
      char* str = strerror_r(-__res_val, buf, sizeof(buf));                      \
      LOG(FATAL) << "Error " << (-__res_val) << " evaluating '" #x "': " << str; \
    }                                                                            \
  } while (false)


using namespace boost;
namespace ctx = boost::context;

namespace util {
namespace epoll {

namespace {


struct signal_state {
  struct Item {
    EvController* ev_controller = nullptr;
    std::function<void(int)> cb;
  };

  Item signal_map[NSIG];
};

signal_state* get_signal_state() {
  static signal_state state;

  return &state;
}

void SigAction(int signal, siginfo_t*, void*) {
  signal_state* state = get_signal_state();
  DCHECK_LT(signal, _NSIG);

  auto& item = state->signal_map[signal];
  auto cb = [signal, &item] { item.cb(signal); };

  if (item.ev_controller && item.cb) {
    item.ev_controller->AsyncFiber(std::move(cb));
  } else {
    LOG(ERROR) << "Tangling signal handler " << signal;
  }
}

inline uint64_t GetClockNanos() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * std::nano::den + ts.tv_nsec;
}


constexpr uint64_t kIgnoreIndex = 0;
constexpr uint64_t kNopIndex = 2;
constexpr uint64_t kUserDataCbIndex = 1024;
constexpr uint32_t kSpinLimit = 100;

}  // namespace

thread_local EvController::TLInfo EvController::tl_info_;

EvController::EvController() : task_queue_(128) {
  event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  CHECK_GE(event_fd_, 0);
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  CHECK_GE(epoll_fd_, 0);
  VLOG(1) << "Created event_fd " << event_fd_ << ", epoll_fd_ " << epoll_fd_;

  volatile ctx::fiber dummy;  // For some weird reason I need this to pull
                              // boost::context into linkage.
}

EvController::~EvController() {
  CHECK(is_stopped_);
  close(event_fd_);
  close(epoll_fd_);

  DVLOG(1) << "~EvController";

  signal_state* ss = get_signal_state();
  for (size_t i = 0; i < ABSL_ARRAYSIZE(ss->signal_map); ++i) {
    if (ss->signal_map[i].ev_controller == this) {
      ss->signal_map[i].ev_controller = nullptr;
      ss->signal_map[i].cb = nullptr;
    }
  }
}

void EvController::Stop() {
  AsyncBrief([this] { is_stopped_ = true; });
  VLOG(1) << "EvController::StopFinish";
}

void EvController::Run() {
  VLOG(1) << "EvController::Run";
  Init();

  main_loop_ctx_ = fibers::context::active();
  fibers::scheduler* sched = main_loop_ctx_->get_scheduler();

  EpollFiberAlgo* algo = new EpollFiberAlgo(this);
  sched->set_algo(algo);
  this_fiber::properties<EpollFiberProps>().set_name("ioloop");

  is_stopped_ = false;

  constexpr size_t kBatchSize = 64;
  struct epoll_event cevents[kBatchSize];

  uint32_t tq_seq = 0;
  uint32_t num_stalls = 0;
  uint32_t spin_loops = 0, num_task_runs = 0;
  Tasklet task;

  while (true) {
    num_task_runs = 0;

    uint64_t task_start = 0;

    tq_seq = tq_seq_.load(std::memory_order_acquire);

    // This should handle wait-free and "submit-free" short CPU tasks enqued using Async/Await
    // calls. We allocate the quota of 500K nsec (500usec) of CPU time per iteration.
    while (task_queue_.try_dequeue(task)) {
      ++num_task_runs;
      tl_info_.monotonic_time = GetClockNanos();
      task();
      if (task_start == 0) {
        task_start = tl_info_.monotonic_time;
      } else if (task_start + 500000 < tl_info_.monotonic_time) {
        break;
      }
    }

    if (num_task_runs) {
      task_queue_avail_.notifyAll();
    }

    int timeout = 0;  // By default we do not block on epoll_wait.

    if (spin_loops >= kSpinLimit) {
      spin_loops = 0;

      if (tq_seq_.compare_exchange_weak(tq_seq, WAIT_SECTION_STATE, std::memory_order_acquire)) {
        // We check stop condition when all the pending events were processed.
        // It's up to the app-user to make sure that the incoming flow of events is stopped before
        // stopping EvController.
        if (is_stopped_)
          break;
        ++num_stalls;
        timeout = -1;  // We gonna block on epoll_wait.
      }
    }

    int epoll_res = epoll_wait(epoll_fd_, cevents, kBatchSize, timeout);
    if (epoll_res < 0) {
      LOG(FATAL) << "TBD: " << errno << " " << strerror(errno);
    }
    uint32_t cqe_count = epoll_res;
    if (cqe_count) {
      DVLOG(2) << "Fetched " << cqe_count << " cqes";
      DispatchCompletions(cevents, cqe_count);
    }

    if (timeout == -1) {
      // Reset all except the LSB bit that signals that we need to switch to dispatch fiber.
      tq_seq_.fetch_and(1, std::memory_order_release);
    }

    if (tq_seq & 1) {  // We allow dispatch fiber to run.
      tq_seq_.fetch_and(~1U, std::memory_order_relaxed);
      this_fiber::yield();
    }

    if (sched->has_ready_fibers()) {
      // Suspend this fiber until others finish runnning and get blocked.
      // Eventually UringFiberAlgo will resume back this fiber in suspend_until
      // function.
      DVLOG(2) << "Suspend ioloop";
      tl_info_.monotonic_time = GetClockNanos();
      sched->suspend();

      DVLOG(2) << "Resume ioloop";
      spin_loops = 0;
    }
    ++spin_loops;
  }

  VLOG(1) << "wakeups/stalls: " << tq_wakeups_.load() << "/" << num_stalls;

  VLOG(1) << "centries size: " << centries_.size();
}

unsigned EvController::Arm(int fd, CbType cb, uint32_t event_mask) {
  epoll_event ev;
  ev.events = event_mask;
  if (next_free_ce_ < 0) {
    RegrowCentries();
    CHECK_GT(next_free_ce_, 0);
  }

  ev.data.u32 = next_free_ce_ + kUserDataCbIndex;
  DCHECK_LT(unsigned(next_free_ce_), centries_.size());

  auto& e = centries_[next_free_ce_];
  DCHECK(!e.cb);  // cb is undefined.
  DVLOG(1) << "Arm: index: " << next_free_ce_;

  unsigned ret = next_free_ce_;
  next_free_ce_ = e.index;
  e.cb = std::move(cb);
  e.index = -1;

  CHECK_EQ(0, epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev));
  return ret;
}

void EvController::UpdateCb(unsigned arm_index, CbType cb) {
  CHECK_LT(arm_index, centries_.size());
  centries_[arm_index].cb = cb;
}

void EvController::Disarm(unsigned arm_index) {
  CHECK_LT(arm_index, centries_.size());
  centries_[arm_index].cb = nullptr;
  centries_[arm_index].index = next_free_ce_;

  next_free_ce_ = arm_index;
}

void EvController::Init() {
  CHECK_EQ(0U, thread_id_) << "Init was already called";

  centries_.resize(512);  // .index = -1
  next_free_ce_ = 0;
  for (size_t i = 0; i < centries_.size() - 1; ++i) {
    centries_[i].index = i + 1;
  }

  thread_id_ = pthread_self();
  tl_info_.is_ev_thread = true;

  Arm(event_fd_, [ev_fd = event_fd_](uint32_t mask, auto*) {
    DVLOG(1) << "EventFdCb called " << mask;
    uint64_t val;
    CHECK_EQ(8, read(ev_fd, &val, sizeof(val)));
  }, EPOLLIN);
}

void EvController::DoWake() {
  DVLOG(1) << "Wake ring " << tq_seq_.load(std::memory_order_relaxed);

  tq_wakeups_.fetch_add(1, std::memory_order_relaxed);

  uint64_t val = 1;
  CHECK_EQ(8, write(event_fd_, &val, sizeof(uint64_t)));
}

void EvController::DispatchCompletions(epoll_event* cevents, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    const auto& cqe = cevents[i];

    // I allocate range of 1024 reserved values for the internal EvController use.
    uint32_t user_data = cqe.data.u32;
    if (cqe.data.u32 >= kUserDataCbIndex) {  // our heap range surely starts higher than 1k.
      size_t index = user_data - kUserDataCbIndex;
      DCHECK_LT(index, centries_.size());
      const auto& item = centries_[index];
      DCHECK(item.cb) << index;


      item.cb(cqe.events, this);
      continue;
    }

    if (user_data == kIgnoreIndex || kNopIndex)
      continue;

    LOG(ERROR) << "Unrecognized user_data " << user_data;
  }
}

void EvController::RegisterSignal(std::initializer_list<uint16_t> l, std::function<void(int)> cb) {
  auto* state = get_signal_state();

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  if (cb) {
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &SigAction;

    for (uint16_t val : l) {
      CHECK(!state->signal_map[val].cb) << "Signal " << val << " was already registered";
      state->signal_map[val].cb = cb;
      state->signal_map[val].ev_controller = this;

      CHECK_EQ(0, sigaction(val, &sa, NULL));
    }
  } else {
    sa.sa_handler = SIG_DFL;

    for (uint16_t val : l) {
      CHECK(state->signal_map[val].cb) << "Signal " << val << " was already registered";
      state->signal_map[val].cb = nullptr;
      state->signal_map[val].ev_controller = nullptr;

      CHECK_EQ(0, sigaction(val, &sa, NULL));
    }
  }
}

void EvController::RegrowCentries() {
  size_t prev = centries_.size();
  VLOG(1) << "RegrowCentries from " << prev << " to " << prev * 2;

  centries_.resize(prev * 2);  // grow by 2.
  next_free_ce_ = prev;
  for (; prev < centries_.size() - 1; ++prev)
    centries_[prev].index = prev + 1;
}


}  // namespace epoll
}  // namespace util
