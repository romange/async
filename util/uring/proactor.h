// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <liburing.h>
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
#include "util/uring/submit_entry.h"

namespace util {
namespace uring {

class UringFiberAlgo;
class ProactorPool;

class Proactor {
  Proactor(const Proactor&) = delete;
  void operator=(const Proactor&) = delete;

 public:
  Proactor();
  ~Proactor();

  // Runs the poll-loop. Stalls the calling thread which will become the "Proactor" thread.
  void Run(unsigned ring_depth = 512, int wq_fd = -1);

  //! Signals proactor to stop. Does not wait for it.
  void Stop();

  using IoResult = int;

  // IoResult is the I/O result of the completion event.
  // int64_t is the payload supplied during event submission. See GetSubmitEntry below.
  using CbType = std::function<void(IoResult, int64_t, Proactor*)>;

  /**
   * @brief Get the Submit Entry object in order to issue I/O request.
   *
   * @param cb - completion callback.
   * @param payload - an argument to the completion callback that is further passed as the second
   *                  argument to CbType(). Can be nullptr if no notification is required.
   * @return SubmitEntry with initialized userdata.
   *
   * This method might block the calling fiber therefore it should not be called within proactor
   * context. In other words it can not be called from  *Brief([]...) calls to Proactor.
   * In addition, this method can not be used for introducing IOSQE_IO_LINK chains since they
   * require atomic SQE allocation.
   * @todo We should add GetSubmitEntries that can allocate multiple SQEs atomically.
   *       In that case we will need RegisterCallback function that takes an unregistered SQE
   *       and assigns a callback to it. GetSubmitEntry will be implemented using those functions.
   */
  SubmitEntry GetSubmitEntry(CbType cb, int64_t payload);

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
    return tl_info_.is_proactor_thread;
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


  bool HasFastPoll() const {
    return fast_poll_f_;
  }

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
  // Proactor main fiber (i.e. Async callbacks).
  template <typename... Args> boost::fibers::fiber LaunchFiber(Args&&... args) {
    ::boost::fibers::fiber fb;

    // It's safe to use & capture since we await before returning.
    AwaitBrief([&] { fb = boost::fibers::fiber(std::forward<Args>(args)...); });
    return fb;
  }

  // Runs possibly awating function 'f' safely in Proactor thread and waits for it to finish,
  // If we are in his thread already, runs 'f' directly, otherwise
  // runs it wrapped in a fiber. Should be used instead of 'AwaitBrief' when 'f' itself
  // awaits on something.
  // To summarize: 'f' may not block its thread, but allowed to block its fiber.
  template <typename Func> auto AwaitBlocking(Func&& f) -> decltype(f());

  void RegisterSignal(std::initializer_list<uint16_t> l, std::function<void(int)> cb);

  void ClearSignal(std::initializer_list<uint16_t> l) {
    RegisterSignal(l, nullptr);
  }

  int ring_fd() const { return ring_.ring_fd;}

 private:
  enum { WAIT_SECTION_STATE = 1UL << 31 };

  void Init(size_t ring_size, int wq_fd = -1);

  void WakeRing();

  void WakeupIfNeeded() {
    auto current = tq_seq_.fetch_add(2, std::memory_order_relaxed);
    if (current == WAIT_SECTION_STATE) {
      WakeRing();
    }
  }

  void DispatchCompletions(io_uring_cqe* cqes, unsigned count);

  template <typename Func> bool EmplaceTaskQueue(Func&& f) {
    if (task_queue_.try_enqueue(std::forward<Func>(f))) {
      WakeupIfNeeded();

      return true;
    }
    return false;
  }

  void RegrowCentries();

  io_uring ring_;
  pthread_t thread_id_ = 0U;

  int wake_fd_;
  bool is_stopped_ = true;
  uint8_t fast_poll_f_ : 1;
  uint8_t reseved_f_ : 7;

  // We use fu2 function to allow moveable semantics.
  using Tasklet =
      fu2::function_base<true /*owns*/, false /*non-copyable*/, fu2::capacity_default,
                         false /* non-throwing*/, false /* strong exceptions guarantees*/, void()>;
  static_assert(sizeof(Tasklet) == 32, "");

  using FuncQ = base::mpmc_bounded_queue<Tasklet>;

  using EventCount = fibers_ext::EventCount;

  FuncQ task_queue_;
  std::atomic_uint32_t tq_seq_{0}, tq_wakeups_{0};
  EventCount task_queue_avail_, sqe_avail_;
  ::boost::fibers::context* main_loop_ctx_ = nullptr;

  friend class UringFiberAlgo;

  struct CompletionEntry {
    CbType cb;

    // serves for linked list management when unused. Also can store an additional payload
    // field when in flight.
    int32_t val = -1;
    int32_t opcode = -1;  // For debugging. TODO: to remove later.
  };
  static_assert(sizeof(CompletionEntry) == 40, "");

  std::vector<CompletionEntry> centries_;
  int32_t next_free_ = -1;

  struct TLInfo {
    bool is_proactor_thread = false;
    uint32_t proactor_index = 0;
    uint64_t monotonic_time = 0;
  };
  static thread_local TLInfo tl_info_;
};



// Implementation
// **********************************************************************
//
template <typename Func> void Proactor::AsyncBrief(Func&& f) {
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

template <typename Func> auto Proactor::AwaitBrief(Func&& f) -> decltype(f()) {
  if (InMyThread()) {
    return f();
  }
  if (IsProactorThread()) {
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
template <typename Func> auto Proactor::AwaitBlocking(Func&& f) -> decltype(f()) {
  if (InMyThread()) {
    return f();
  }

  using ResultType = decltype(f());
  detail::ResultMover<ResultType> mover;
  auto fb = LaunchFiber([&] { mover.Apply(std::forward<Func>(f)); });
  fb.join();

  return std::move(mover).get();
}

}  // namespace uring
}  // namespace util
