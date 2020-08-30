// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <pthread.h>

#include <boost/fiber/fiber.hpp>
#include <functional>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress"
#include "base/function2.hpp"
#pragma GCC diagnostic pop

#include "base/mpmc_bounded_queue.h"
#include "util/fibers/event_count.h"
#include "util/fibers/fibers_ext.h"

struct epoll_event;

namespace util {
namespace epoll {

class EpollFiberAlgo;

class EvController {
  EvController(const EvController&) = delete;
  void operator=(const EvController&) = delete;

 public:
  EvController();
  ~EvController();

  // Runs the poll-loop. Stalls the calling thread which will become the "EvController" thread.
  void Run();

  //! Signals EvController to stop. Does not wait for it.
  void Stop();

  using IoResult = int;


  // event_mask passed from epoll_event.events.
  using CbType = std::function<void(uint32_t event_mask, EvController*)>;

  /**
   * @brief Returns true if the called is running in this EvController thread.
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

  static bool IsEvControllerThread() {
    return tl_info_.is_ev_thread;
  }

  // Returns an approximate (cached) time with nano-sec granularity.
  // The caller must run in the same thread as the EvController.
  static uint64_t GetMonotonicTimeNs() {
    return tl_info_.monotonic_time;
  }

  // Returns an 0 <= index < N, where N is the number of EvController threads in the pool of called
  // from EvController thread. Returns -1 if called from some other thread.
  static int32_t GetIndex() {
    return tl_info_.ev_index;
  }

  static void SetIndex(uint32_t index) {
    tl_info_.ev_index = index;
  }


  unsigned Arm(int fd, CbType cb,  uint32_t event_mask);
  void UpdateCb(unsigned arm_index, CbType cb);
  void Disarm(unsigned arm_index);

  /**
   *  Message passing functions.
   * */

  //! Fire and forget - does not wait for the function to be called.
  //! `f` should not block, lock on mutexes or Await.
  //! Might block the calling fiber if the queue is full.
  template <typename Func> void AsyncBrief(Func&& brief);

  //! Similarly to AsyncBrief but waits 'f' to return.
  template <typename Func> auto AwaitBrief(Func&& brief) -> decltype(brief());

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
  // EvController main fiber (i.e. Async callbacks).
  template <typename... Args> boost::fibers::fiber LaunchFiber(Args&&... args) {
    ::boost::fibers::fiber fb;

    // It's safe to use & capture since we await before returning.
    AwaitBrief([&] { fb = boost::fibers::fiber(std::forward<Args>(args)...); });
    return fb;
  }

  // Runs possibly awating function 'f' safely in EvController thread and waits for it to finish,
  // If we are in his thread already, runs 'f' directly, otherwise
  // runs it wrapped in a fiber. Should be used instead of 'AwaitBrief' when 'f' itself
  // awaits on something.
  // To summarize: 'f' may not block its thread, but allowed to block its fiber.
  template <typename Func> auto AwaitBlocking(Func&& f) -> decltype(f());

  void RegisterSignal(std::initializer_list<uint16_t> l, std::function<void(int)> cb);

  void ClearSignal(std::initializer_list<uint16_t> l) {
    RegisterSignal(l, nullptr);
  }

  int ev_loop_fd() const {
    return epoll_fd_;
  }

 private:
  enum { WAIT_SECTION_STATE = 1UL << 31 };

  void Init();

  void DispatchCompletions(struct epoll_event* cevents, unsigned count);

  // Called only from external threads.
  void DoWake();

  void WakeupIfNeeded() {
    auto current = tq_seq_.fetch_add(2, std::memory_order_relaxed);
    if (current == WAIT_SECTION_STATE) {
      // We protect DoWake using tq_seq_. That means only one thread at a time
      // can enter here. Moreover tq_seq_ == WAIT_SECTION_STATE only when
      // EvController enters WAIT section, therefore we do not race over SQE ring with EvController thread.
      DoWake();
    }
  }

  template <typename Func> bool EmplaceTaskQueue(Func&& f) {
    if (task_queue_.try_enqueue(std::forward<Func>(f))) {
      WakeupIfNeeded();

      return true;
    }
    return false;
  }

  void RegrowCentries();
  void ArmWakeupEvent();

  pthread_t thread_id_ = 0U;

  int event_fd_ = -1, epoll_fd_ = -1;
  bool is_stopped_ = true;

  // We use fu2 function to allow moveable semantics.
  using Tasklet =
      fu2::function_base<true /*owns*/, false /*non-copyable*/, fu2::capacity_default,
                         false /* non-throwing*/, false /* strong exceptions guarantees*/, void()>;
  static_assert(sizeof(Tasklet) == 32, "");

  using FuncQ = base::mpmc_bounded_queue<Tasklet>;

  using EventCount = fibers_ext::EventCount;

  FuncQ task_queue_;
  std::atomic_uint32_t tq_seq_{0}, tq_wakeups_{0};
  EventCount task_queue_avail_;
  ::boost::fibers::context* main_loop_ctx_ = nullptr;

  friend class EpollFiberAlgo;

  struct CompletionEntry {
    CbType cb;

    // serves for linked list management when unused. Also can store an additional payload
    // field when in flight.
    int32_t index = -1;
    int32_t unused = -1;
  };
  static_assert(sizeof(CompletionEntry) == 40, "");

  std::vector<CompletionEntry> centries_;
  int32_t next_free_ce_ = -1;

  struct TLInfo {
    bool is_ev_thread = false;
    uint32_t ev_index = 0;
    uint64_t monotonic_time = 0;
  };
  static thread_local TLInfo tl_info_;
};


// Implementation
// **********************************************************************
//
template <typename Func> void EvController::AsyncBrief(Func&& f) {
  if (EmplaceTaskQueue(std::forward<Func>(f)))
    return;

  while (true) {
    EventCount::Key key = task_queue_avail_.prepareWait();

    if (EmplaceTaskQueue(std::forward<Func>(f))) {
      break;
    }
    task_queue_avail_.wait(key.epoch());
  }
}

template <typename Func> auto EvController::AwaitBrief(Func&& f) -> decltype(f()) {
  if (InMyThread()) {
    return f();
  }
  if (IsEvControllerThread()) {
    // TODO:
  }

  fibers_ext::Done done;
  using ResultType = decltype(f());
  detail::ResultMover<ResultType> mover;

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
template <typename Func> auto EvController::AwaitBlocking(Func&& f) -> decltype(f()) {
  if (InMyThread()) {
    return f();
  }

  using ResultType = decltype(f());
  detail::ResultMover<ResultType> mover;
  auto fb = LaunchFiber([&] { mover.Apply(std::forward<Func>(f)); });
  fb.join();

  return std::move(mover).get();
}

}  // namespace epoll
}  // namespace util
