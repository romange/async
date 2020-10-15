// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/proactor.h"

#include <liburing.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/poll.h>

#include <boost/fiber/operations.hpp>
#include <boost/fiber/scheduler.hpp>

#include "absl/base/attributes.h"
#include "absl/time/clock.h"
#include "base/logging.h"
#include "base/proc_util.h"
#include "util/uring/fiber_socket.h"
#include "util/uring/uring_fiber_algo.h"

DEFINE_bool(proactor_register_fd, false, "If true tries to register file destricptors");

#define URING_CHECK(x)                                                           \
  do {                                                                           \
    int __res_val = (x);                                                         \
    if (ABSL_PREDICT_FALSE(__res_val < 0)) {                                     \
      char buf[128];                                                             \
      char* str = strerror_r(-__res_val, buf, sizeof(buf));                      \
      LOG(FATAL) << "Error " << (-__res_val) << " evaluating '" #x "': " << str; \
    }                                                                            \
  } while (false)

#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif

#define VPRO(verbosity) VLOG(verbosity) << "PRO[" << tl_info_.proactor_index << "] "

using namespace boost;
namespace ctx = boost::context;

namespace util {
namespace uring {

namespace {

inline int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags,
                              sigset_t* sig) {
  return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, _NSIG / 8);
}

ABSL_ATTRIBUTE_NOINLINE void wait_for_cqe(io_uring* ring, unsigned wait_nr, sigset_t* sig = NULL) {
  // res must be 0 or -1.
  int res = sys_io_uring_enter(ring->ring_fd, 0, wait_nr, IORING_ENTER_GETEVENTS, sig);
  if (res == 0 || errno == EINTR)
    return;
  DCHECK_EQ(-1, res);
  res = errno;

  LOG(FATAL) << "Error " << (res) << " evaluating sys_io_uring_enter: " << strerror(res);
}

inline uint64_t GetClockNanos() {
  return absl::GetCurrentTimeNanos();
}

inline unsigned CQReadyCount(const io_uring& ring) {
  return io_uring_smp_load_acquire(ring.cq.ktail) - *ring.cq.khead;
}

unsigned IoRingPeek(const io_uring& ring, io_uring_cqe* cqes, unsigned count) {
  unsigned ready = CQReadyCount(ring);
  if (!ready)
    return 0;

  count = count > ready ? ready : count;
  unsigned head = *ring.cq.khead;
  unsigned mask = *ring.cq.kring_mask;
  unsigned last = head + count;
  for (int i = 0; head != last; head++, i++) {
    cqes[i] = ring.cq.cqes[head & mask];
  }
  return count;
}

constexpr uint64_t kIgnoreIndex = 0;
constexpr uint64_t kWakeIndex = 1;

constexpr uint64_t kUserDataCbIndex = 1024;

// Important to spin a bit, otherwise we put too much pressure on  eventfd_write.
constexpr uint32_t kSpinLimit = 10;

}  // namespace


Proactor::Proactor() : ProactorBase() {}

Proactor::~Proactor() {
  idle_map_.clear();

  CHECK(is_stopped_);
  if (thread_id_ != -1U) {
    io_uring_queue_exit(&ring_);
  }
  VLOG(1) << "Closing wake_fd " << wake_fd_ << " ring fd: " << ring_.ring_fd;
}

void Proactor::Run() {
  VLOG(1) << "Proactor::Run";

  main_loop_ctx_ = fibers::context::active();
  fibers::scheduler* sched = main_loop_ctx_->get_scheduler();

  UringFiberAlgo* scheduler = new UringFiberAlgo(this);
  sched->set_algo(scheduler);
  this_fiber::properties<FiberProps>().set_name("ioloop");

  is_stopped_ = false;

  constexpr size_t kBatchSize = 64;
  struct io_uring_cqe cqes[kBatchSize];
  uint32_t tq_seq = 0;
  uint64_t num_stalls = 0, cqe_syscalls = 0, cqe_fetches = 0, loop_cnt = 0;

  uint32_t spin_loops = 0, num_task_runs = 0, task_interrupts = 0;
  Tasklet task;

  while (true) {
    ++loop_cnt;

    int num_submitted = io_uring_submit(&ring_);

    if (num_submitted >= 0) {
      if (num_submitted)
        DVLOG(2) << "Submitted " << num_submitted;
    } else if (num_submitted == -EBUSY) {
      VLOG(1) << "EBUSY " << num_submitted;
      num_submitted = 0;
    } else {
      URING_CHECK(num_submitted);
    }

    num_task_runs = 0;

    tq_seq = tq_seq_.load(std::memory_order_acquire);

    // This should handle wait-free and "brief" CPU-only tasks enqued using Async/Await
    // calls. We allocate the quota of 500K nsec (500usec) of CPU time per iteration
    // To save redundant timer-calls we start measuring time only when if the queue is not empty.
    if (task_queue_.try_dequeue(task)) {
      uint64_t task_start = GetClockNanos();
      // update thread-local clock service via GetMonotonicTimeNs().
      tl_info_.monotonic_time = task_start;
      do {
        task();
        ++num_task_runs;
        tl_info_.monotonic_time = GetClockNanos();
        if (task_start + 500000 < tl_info_.monotonic_time) {  // Break after 500usec
          ++task_interrupts;
          break;
        }
      } while (task_queue_.try_dequeue(task));

      task_queue_avail_.notifyAll();

      DVLOG(2) << "Tasks runs " << num_task_runs << "/" << spin_loops;
    }

    uint32_t cqe_count = IoRingPeek(ring_, cqes, kBatchSize);
    if (cqe_count) {
      // Once we copied the data we can mark the cqe consumed.
      io_uring_cq_advance(&ring_, cqe_count);
      DVLOG(2) << "Fetched " << cqe_count << " cqes";
      DispatchCompletions(cqes, cqe_count);
      sqe_avail_.notifyAll();
    }

    if (tq_seq & 1) {  // We allow dispatch fiber to run.
      tq_seq_.fetch_and(~1, std::memory_order_relaxed);
      this_fiber::yield();
    }

    if (sched->has_ready_fibers()) {
      // Suspend this fiber until others will run and get blocked.
      // Eventually UringFiberAlgo will resume back this fiber in suspend_until
      // function.
      DVLOG(2) << "Suspend ioloop";
      tl_info_.monotonic_time = GetClockNanos();
      sched->suspend();

      DVLOG(2) << "Resume ioloop";

      continue;
    }

    if (cqe_count || io_uring_sq_ready(&ring_) || !task_queue_.empty()) {
      spin_loops = 0;
      continue;
    }

    ++cqe_syscalls;
    wait_for_cqe(&ring_, 0);  // nonblocking syscall to dive into kernel space.
    if (CQReadyCount(ring_)) {
      ++cqe_fetches;
      spin_loops = 0;
      continue;
    }

    if (!idle_map_.empty()) {  // TODO: to break upon timer constraints (~20usec).
      for (auto it = idle_map_.begin(); it != idle_map_.end(); ++it) {
        if (!it->second()) {
          idle_map_.erase(it);
          break;
        }
      }
      continue;
    }

    // Lets spin a bit to make a system a bit more responsive.
    if (++spin_loops < kSpinLimit) {
      // We should not spin too much using sched_yield or it burns a fuckload of cpu.
      continue;
    }

    spin_loops = 0;  // Reset the spinning.
    // pthread_yield();

    /**
     * If tq_seq_ has changed since it was cached into tq_seq, then
     * EmplaceTaskQueue succeeded and we might have more tasks to execute - lets
     * run the loop again. Otherwise, set tq_seq_ to WAIT_SECTION_STATE, hinting that
     * we are going to stall now. Other threads will need to wake-up the ring
     * (see WakeRing()) but only one will actually call the syscall.
     */
    if (tq_seq_.compare_exchange_weak(tq_seq, WAIT_SECTION_STATE, std::memory_order_acquire)) {
      if (is_stopped_)
        break;
      DVLOG(2) << "wait_for_cqe";
      wait_for_cqe(&ring_, 1);
      DVLOG(2) << "Woke up " << tq_seq_.load(std::memory_order_acquire);

      tq_seq = 0;
      ++num_stalls;

      // Reset all except the LSB bit that signals that we need to switch to dispatch fiber.
      tq_seq_.fetch_and(1, std::memory_order_release);
    }
  }

  VPRO(1) << "total/stalls/cqe_syscalls/cqe_fetches: " << loop_cnt << "/" << num_stalls << "/"
          << cqe_syscalls << "/" << cqe_fetches;

  VPRO(1) << "tq_wakeups/tq_full/tq_task_int/algo_notifies: " << tq_wakeup_ev_.load() << "/"
          << tq_full_ev_.load() << "/" << task_interrupts << "/" << algo_notify_cnt_.load();

  VLOG(1) << "centries size: " << centries_.size();
  centries_.clear();
}


FiberSocketBase* Proactor::CreateSocket() {
  return new FiberSocket{this};
}

void Proactor::Init(size_t ring_size, int wq_fd) {
  CHECK_EQ(0U, ring_size & (ring_size - 1));
  CHECK_GE(ring_size, 8U);
  CHECK_EQ(0U, thread_id_) << "Init was already called";

  base::sys::KernelVersion kver;
  base::sys::GetKernelVersion(&kver);

  CHECK(kver.kernel > 5 || (kver.kernel = 5 && kver.major >= 4))
      << "Versions 5.4 or higher are supported";

  io_uring_params params;
  memset(&params, 0, sizeof(params));

  if (FLAGS_proactor_register_fd && geteuid() == 0) {
    params.flags |= IORING_SETUP_SQPOLL;
    LOG_FIRST_N(INFO, 1) << "Root permissions - setting SQPOLL flag";
  }

  // Optionally reuse the already created work-queue from another uring.
  if (wq_fd > 0) {
    params.flags |= IORING_SETUP_ATTACH_WQ;
    params.wq_fd = wq_fd;
  }

  // it seems that SQPOLL requires registering each fd, including sockets fds.
  // need to check if its worth pursuing.
  // For sure not in short-term.
  // params.flags = IORING_SETUP_SQPOLL;
  VLOG(1) << "Create uring of size " << ring_size;

  // If this fails with 'can not allocate memory' most probably you need to increase maxlock limit.
  URING_CHECK(io_uring_queue_init_params(ring_size, &ring_, &params));
  fast_poll_f_ = (params.features & IORING_FEAT_FAST_POLL) != 0;
  sqpoll_f_ = (params.flags & IORING_SETUP_SQPOLL) != 0;

  wake_fixed_fd_ = wake_fd_;
  register_fd_ = FLAGS_proactor_register_fd;
  if (register_fd_) {
    register_fds_.resize(64, -1);
    register_fds_[0] = wake_fd_;
    wake_fixed_fd_ = 0;

    absl::Time start = absl::Now();
    int res = io_uring_register_files(&ring_, register_fds_.data(), register_fds_.size());
    absl::Duration duration = absl::Now() - start;
    VLOG(1) << "io_uring_register_files took " << absl::ToInt64Milliseconds(duration) << "ms";
    if (res < 0) {
      register_fd_ = 0;
      register_fds_ = decltype(register_fds_){};
    }
  }

  if (!fast_poll_f_) {
    LOG_FIRST_N(INFO, 1) << "IORING_FEAT_FAST_POLL feature is not present in the kernel";
  }

  if (0 == (params.features & IORING_FEAT_NODROP)) {
    LOG_FIRST_N(INFO, 1) << "IORING_FEAT_NODROP feature is not present in the kernel";
  }

  if (params.features & IORING_FEAT_SINGLE_MMAP) {
    size_t sz = ring_.sq.ring_sz + params.sq_entries * sizeof(struct io_uring_sqe);
    LOG_FIRST_N(INFO, 1) << "IORing with " << params.sq_entries << " allocated " << sz
                         << " bytes, cq_entries is " << *ring_.cq.kring_entries;
  }
  CHECK_EQ(ring_size, params.sq_entries);  // Sanity.

  CheckForTimeoutSupport();
  ArmWakeupEvent();
  centries_.resize(params.sq_entries);  // .val = -1
  next_free_ce_ = 0;
  for (size_t i = 0; i < centries_.size() - 1; ++i) {
    centries_[i].val = i + 1;
  }

  thread_id_ = pthread_self();
  tl_info_.owner = this;
}

// Remember, WakeRing is called from external threads.
void Proactor::WakeRing() {
  DVLOG(2) << "Wake ring " << tq_seq_.load(std::memory_order_relaxed);

  tq_wakeup_ev_.fetch_add(1, std::memory_order_relaxed);

  /**
   * It's tempting to use io_uring_prep_nop() here in order to resume wait_cqe() call.
   * However, it's not that staightforward. io_uring_get_sqe and io_uring_submit
   * are not thread-safe and this function is called from another thread.
   * Even though tq_seq_ == WAIT_SECTION_STATE ensured that Proactor thread
   * is going to stall we can not guarantee that it will not wake up before we reach the next line.
   * In that case, Proactor loop will continue and both threads could call
   * io_uring_get_sqe and io_uring_submit at the same time. This will cause data-races.
   * It's possible to fix this by guarding with spinlock the section below as well as
   * the section after the wait_cqe() call but I think it's overcomplicated and not worth it.
   * Therefore we gonna stick with event_fd descriptor to wake up Proactor thread.
   */

  uint64_t val = 1;
  CHECK_EQ(8, write(wake_fd_, &val, sizeof(uint64_t)));
}

void Proactor::DispatchCompletions(io_uring_cqe* cqes, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    auto& cqe = cqes[i];

    // I allocate range of 1024 reserved values for the internal Proactor use.

    if (cqe.user_data >= kUserDataCbIndex) {  // our heap range surely starts higher than 1k.
      size_t index = cqe.user_data - kUserDataCbIndex;
      DCHECK_LT(index, centries_.size());
      auto& e = centries_[index];
      DCHECK(e.cb) << index;

      CbType func;
      auto payload = e.val;
      func.swap(e.cb);

      // Set e to be the head of free-list.
      e.val = next_free_ce_;
      next_free_ce_ = index;

      func(cqe.res, payload, this);
      continue;
    }

    if (cqe.user_data == kIgnoreIndex)
      continue;

    if (cqe.user_data == kWakeIndex) {
      // We were woken up. Need to rearm wake_fd_ poller.
      DCHECK_EQ(cqe.res, 8);
      DVLOG(2) << "PRO[" << tl_info_.proactor_index << "] Wakeup " << cqe.res << "/" << cqe.flags;

      // TODO: to move io_uring_get_sqe call from here to before we stall.
      ArmWakeupEvent();
      continue;
    }
    LOG(ERROR) << "Unrecognized user_data " << cqe.user_data;
  }
}

SubmitEntry Proactor::GetSubmitEntry(CbType cb, int64_t payload) {
  io_uring_sqe* res = io_uring_get_sqe(&ring_);
  if (res == NULL) {
    fibers::context* current = fibers::context::active();
    CHECK(current != main_loop_ctx_) << "SQE overflow in the main context";

    sqe_avail_.await([this] { return io_uring_sq_space_left(&ring_) > 0; });
    res = io_uring_get_sqe(&ring_);  // now we should have the space.
    CHECK(res);
  }

  if (cb) {
    if (next_free_ce_ < 0) {
      RegrowCentries();
      DCHECK_GT(next_free_ce_, 0);
    }

    res->user_data = next_free_ce_ + kUserDataCbIndex;
    DCHECK_LT(unsigned(next_free_ce_), centries_.size());

    auto& e = centries_[next_free_ce_];
    DCHECK(!e.cb);  // cb is undefined.
    DVLOG(2) << "GetSubmitEntry: index: " << next_free_ce_ << ", socket: " << payload;

    next_free_ce_ = e.val;
    e.cb = std::move(cb);
    e.val = payload;
    e.opcode = -1;
  } else {
    res->user_data = kIgnoreIndex;
  }

  return SubmitEntry{res};
}

void Proactor::RegrowCentries() {
  size_t prev = centries_.size();
  VLOG(1) << "RegrowCentries from " << prev << " to " << prev * 2;

  centries_.resize(prev * 2);  // grow by 2.
  next_free_ce_ = prev;
  for (; prev < centries_.size() - 1; ++prev)
    centries_[prev].val = prev + 1;
}

void Proactor::ArmWakeupEvent() {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  CHECK_NOTNULL(sqe);

  io_uring_prep_poll_add(sqe, wake_fixed_fd_, POLLIN);
  sqe->user_data = kIgnoreIndex;
  sqe->flags |= (register_fd_ ? IOSQE_FIXED_FILE : 0);
  sqe->flags |= IOSQE_IO_LINK;
  sqe = io_uring_get_sqe(&ring_);

  // drain the signal.
  static uint64_t donot_care;
  io_uring_prep_read(sqe, wake_fixed_fd_, &donot_care, 8, 0);
  sqe->user_data = kWakeIndex;
}

unsigned Proactor::RegisterFd(int source_fd) {
  if (!register_fd_)
    return source_fd;

  auto next = std::find(register_fds_.begin() + next_free_fd_, register_fds_.end(), -1);
  if (next == register_fds_.end()) {
    size_t prev_sz = register_fds_.size();
    register_fds_.resize(prev_sz * 2, -1);
    register_fds_[prev_sz] = source_fd;
    next_free_fd_ = prev_sz + 1;

    CHECK_EQ(0, io_uring_register_files(&ring_, register_fds_.data(), register_fds_.size()));
    return prev_sz;
  }

  *next = source_fd;
  next_free_fd_ = next - register_fds_.begin();
  CHECK_EQ(1, io_uring_register_files_update(&ring_, next_free_fd_, &source_fd, 1));
  ++next_free_fd_;

  return next_free_fd_ - 1;
}

void Proactor::UnregisterFd(unsigned fixed_fd) {
  if (!register_fd_)
    return;

  CHECK_LT(fixed_fd, register_fds_.size());
  CHECK_GE(register_fds_[fixed_fd], 0);
  register_fds_[fixed_fd] = -1;

  CHECK_EQ(1, io_uring_register_files_update(&ring_, fixed_fd, &register_fds_[fixed_fd], 1));
  if (fixed_fd < next_free_fd_) {
    next_free_fd_ = fixed_fd;
  }
}

void Proactor::CheckForTimeoutSupport() {
  io_uring_sqe* sqe = CHECK_NOTNULL(io_uring_get_sqe(&ring_));
  timespec ts = {.tv_sec = 0, .tv_nsec = 10000};
  static_assert(sizeof(__kernel_timespec) == sizeof(timespec));

  io_uring_prep_timeout(sqe, (__kernel_timespec*)&ts, 0, IORING_TIMEOUT_ABS);
  sqe->user_data = 2;
  int submitted = io_uring_submit(&ring_);
  CHECK_EQ(1, submitted);

  io_uring_cqe* cqe = nullptr;
  CHECK_EQ(0, io_uring_wait_cqe(&ring_, &cqe));
  CHECK_EQ(2U, cqe->user_data);

  // AL2 5.4 does not support timeout. Ubuntu 5.4 supports only relative option.
  // We can not use timeout API for 5.4.
  if (cqe->res < 0 && cqe->res != -ETIME) {
    support_timeout_ = 0;
    VLOG(1) << "Timeout op is not supported " << -cqe->res;
  } else {
    CHECK(cqe->res == -ETIME || cqe->res == 0);
    support_timeout_ = 1;
  }
  io_uring_cq_advance(&ring_, 1);
}

}  // namespace uring
}  // namespace util
