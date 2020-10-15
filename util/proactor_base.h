// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#pragma once

#include <pthread.h>

#include <boost/fiber/fiber.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress"
#include "base/function2.hpp"
#pragma GCC diagnostic pop

#include <absl/container/flat_hash_map.h>

#include <functional>

#include "base/mpmc_bounded_queue.h"
#include "util/fibers/fibers_ext.h"

namespace util {

class FiberSchedAlgo;
class FiberSocketBase;

class ProactorBase {
  ProactorBase(const ProactorBase&) = delete;
  void operator=(const ProactorBase&) = delete;
  friend class FiberSchedAlgo;

 public:
  ProactorBase();
  virtual ~ProactorBase();

  // Runs the poll-loop. Stalls the calling thread which will become the "Proactor" thread.
  virtual void Run() = 0;

  //! Signals proactor to stop. Does not wait for it.
  void Stop();

  //! Creates a socket that can be used with this proactor.
  virtual FiberSocketBase* CreateSocket() = 0;

  /**
   * @brief Returns true if the called is running in this Proactor thread.
   *
   * @return true
   * @return false
   */
  bool InMyThread() const {
    return pthread_self() == thread_id_;
  }

  auto thread_id() const {
    return thread_id_;
  }

  static bool IsProactorThread() {
    return tl_info_.owner != nullptr;
  }

  static ProactorBase* me() {
    return tl_info_.owner;
  }

  void RegisterSignal(std::initializer_list<uint16_t> l, std::function<void(int)> cb);

  void ClearSignal(std::initializer_list<uint16_t> l) {
    RegisterSignal(l, nullptr);
  }


  // Returns an approximate (cached) time with nano-sec granularity.
  // The caller must run in the same thread as the proactor.
  static uint64_t GetMonotonicTimeNs() {
    return tl_info_.monotonic_time;
  }

  // Returns an 0 <= index < N, where N is the number of proactor threads in the pool of called
  // from Proactor thread. Returns -1 if called from some other thread.
  static int32_t GetIndex() {
    return tl_info_.proactor_index;
  }

  // Internal, used by ProactorPool
  static void SetIndex(uint32_t index) {
    tl_info_.proactor_index = index;
  }

  //! Fire and forget - does not wait for the function to be called.
  //! `f` should not block, lock on mutexes or Await.
  //! Might block the calling fiber if the queue is full.
  template <typename Func> void AsyncBrief(Func&& brief);

  //! Similarly to AsyncBrief but waits 'f' to return.
  template <typename Func> auto AwaitBrief(Func&& brief) -> decltype(brief());

  // Runs possibly awating function 'f' safely in Proactor thread and waits for it to finish,
  // If we are in his thread already, runs 'f' directly, otherwise
  // runs it wrapped in a fiber. Should be used instead of 'AwaitBrief' when 'f' itself
  // awaits on something.
  // To summarize: 'f' may not block its thread, but allowed to block its fiber.
  template <typename Func> auto AwaitBlocking(Func&& f) -> decltype(f());

  //! Similarly to AsyncBrief but 'f' but wraps 'f' in fiber.
  //! f is allowed to fiber-block or await.
  template <typename Func, typename... Args> void AsyncFiber(Func&& f, Args&&... args) {
    // Ideally we want to forward args into lambda but it's too complicated before C++20.
    // So I just copy them into capture.
    // We forward captured variables so we need lambda to be mutable.
    AsyncBrief([f = std::forward<Func>(f), args...]() mutable {
      ::boost::fibers::fiber(std::forward<Func>(f), std::forward<Args>(args)...).detach();
    });
  }

  // Please note that this function uses Await, therefore can not be used inside
  // Proactor main fiber (i.e. Async callbacks).
  template <typename... Args> boost::fibers::fiber LaunchFiber(Args&&... args) {
    ::boost::fibers::fiber fb;

    // It's safe to use & capture since we await before returning.
    AwaitBrief([&] { fb = boost::fibers::fiber(std::forward<Args>(args)...); });
    return fb;
  }

  using IdleTask = std::function<bool()>;

  /**
   * @brief Adds a task that should run when Proactor loop is idle. The task should return
   *        true if keep it running or false if it finished its job.
   *
   * @tparam Func
   * @param f
   * @return uint64_t an unique ids denoting this task. Can be used for cancellation.
   */
  uint64_t AddIdleTask(IdleTask f);

 protected:
  enum { WAIT_SECTION_STATE = 1UL << 31 };

  // Called only from external threads.
  void WakeRing();

  void WakeupIfNeeded();

  pthread_t thread_id_ = 0U;
  int wake_fd_;
  bool is_stopped_ = true;

  std::atomic_uint32_t tq_seq_{0}, tq_full_ev_{0};
  std::atomic_uint32_t tq_wakeup_ev_{0};
  std::atomic_uint32_t algo_notify_cnt_{0} /* how many times this FiberAlgo woke up others */;

  // We use fu2 function to allow moveable semantics.
  using Fu2Fun =
      fu2::function_base<true /*owns*/, false /*non-copyable*/, fu2::capacity_default,
                         false /* non-throwing*/, false /* strong exceptions guarantees*/, void()>;
  struct Tasklet : public Fu2Fun {
    using Fu2Fun::Fu2Fun;
    using Fu2Fun::operator=;
  };
  static_assert(sizeof(Tasklet) == 32, "");

  using FuncQ = base::mpmc_bounded_queue<Tasklet>;

  using EventCount = fibers_ext::EventCount;

  FuncQ task_queue_;
  EventCount task_queue_avail_;

  uint64_t next_idle_task_{1};
  absl::flat_hash_map<uint64_t, IdleTask> idle_map_;
  absl::flat_hash_map<uint64_t, IdleTask>::const_iterator idle_it_;

  struct TLInfo {
    uint32_t proactor_index = 0;
    uint64_t monotonic_time = 0;  // in nanoseconds
    ProactorBase* owner = nullptr;
  };
  static thread_local TLInfo tl_info_;

 private:
  template <typename Func> bool EmplaceTaskQueue(Func&& f) {
    if (task_queue_.try_enqueue(std::forward<Func>(f))) {
      WakeupIfNeeded();

      return true;
    }
    return false;
  }
};

// Implementation
// **********************************************************************
//

inline void ProactorBase::WakeupIfNeeded() {
  auto current = tq_seq_.fetch_add(2, std::memory_order_relaxed);
  if (current == WAIT_SECTION_STATE) {
    // We protect WakeRing using tq_seq_. That means only one thread at a time
    // can enter here. Moreover tq_seq_ == WAIT_SECTION_STATE only when
    // proactor enters WAIT section, therefore we do not race over SQE ring with proactor thread.
    std::atomic_thread_fence(std::memory_order_acquire);
    WakeRing();
  }
}

template <typename Func> void ProactorBase::AsyncBrief(Func&& f) {
  if (EmplaceTaskQueue(std::forward<Func>(f)))
    return;

  tq_full_ev_.fetch_add(1, std::memory_order_relaxed);

  // If EventCount::Wait appears on profiler radar, it's most likely because the task queue is
  // too overloaded. It's either the CPU is overloaded or we are sending too many tasks through it.
  while (true) {
    EventCount::Key key = task_queue_avail_.prepareWait();

    if (EmplaceTaskQueue(std::forward<Func>(f))) {
      break;
    }
    task_queue_avail_.wait(key.epoch());
  }
}

template <typename Func> auto ProactorBase::AwaitBrief(Func&& f) -> decltype(f()) {
  if (InMyThread()) {
    return f();
  }
  if (IsProactorThread()) {
    // TODO:
  }

  fibers_ext::Done done;
  using ResultType = decltype(f());
  fibers_ext::detail::ResultMover<ResultType> mover;

  // Store done-ptr by value to increase the refcount while lambda is running.
  AsyncBrief([&mover, f = std::forward<Func>(f), done]() mutable {
    mover.Apply(f);
    done.Notify();
  });

  done.Wait();
  return std::move(mover).get();
}

// Runs possibly awating function 'f' safely in ContextThread and waits for it to finish,
// If we are in the context thread already, runs 'f' directly, otherwise
// runs it wrapped in a fiber. Should be used instead of 'Await' when 'f' itself
// awaits on something.
// To summarize: 'f' should not block its thread, but allowed to block its fiber.
template <typename Func> auto ProactorBase::AwaitBlocking(Func&& f) -> decltype(f()) {
  if (InMyThread()) {
    return f();
  }

  using ResultType = decltype(f());
  fibers_ext::detail::ResultMover<ResultType> mover;
  auto fb = LaunchFiber([&] { mover.Apply(std::forward<Func>(f)); });
  fb.join();

  return std::move(mover).get();
}

}  // namespace util
