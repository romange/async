// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
// Based on the design of folly event_count which in turn based on
// Dmitry Vyukov's proposal at
// https://software.intel.com/en-us/forums/intel-threading-building-blocks/topic/299245
#pragma once

#include <absl/base/macros.h>
#include <boost/fiber/context.hpp>


namespace util {
namespace fibers_ext {

// This class is all about reducing the contention on the producer side (notifications).
// We want notifications to be as light as possible, while waits are less important
// since they on the path of being suspended anyway. However, we also want to reduce number of
// spurious waits on the consumer side.
// This class has another wonderful property: notification thread does not need to lock mutex,
// which means it can be used from the io_context (ring0) fiber.
class EventCount {
  using spinlock_lock_t = ::boost::fibers::detail::spinlock_lock;
  using wait_queue_t = ::boost::fibers::context::wait_queue_t;

  //! Please note that we must use spinlock_lock_t because we suspend and unlock atomically
  // and fibers lib supports only this type for that.
  ::boost::fibers::detail::spinlock wait_queue_splk_{};
  wait_queue_t wait_queue_{};

 public:
  EventCount() noexcept : val_(0) {}

  class Key {
    friend class EventCount;
    EventCount* me_;
    uint32_t epoch_;

    explicit Key(EventCount* me, uint32_t e) noexcept : me_(me), epoch_(e) {}

    Key(const Key&) = delete;

   public:
    Key(Key&&) noexcept = default;

    ~Key() {
      me_->val_.fetch_sub(kAddWaiter, std::memory_order_relaxed);
    }

    uint32_t epoch() const { return epoch_; }
  };

  // Return true if a notification was made, false if no notification was issued.
  bool notify() noexcept;

  bool notifyAll() noexcept;

  Key prepareWait() noexcept {
    uint64_t prev = val_.fetch_add(kAddWaiter, std::memory_order_acq_rel);
    return Key(this, prev >> kEpochShift);
  }

  void wait(uint32_t epoch) noexcept;

  /**
   * Wait for condition() to become true.  Will clean up appropriately if
   * condition() throws. Returns true if had to preempt using wait_queue.
   */
  template <typename Condition> bool await(Condition condition);

 private:
  friend class Key;

  static bool should_switch(::boost::fibers::context* ctx, std::intptr_t expected) {
    return ctx->twstatus.compare_exchange_strong(expected, static_cast<std::intptr_t>(-1),
                                                 std::memory_order_acq_rel) ||
           expected == 0;
  }

  EventCount(const EventCount&) = delete;
  EventCount(EventCount&&) = delete;
  EventCount& operator=(const EventCount&) = delete;
  EventCount& operator=(EventCount&&) = delete;

  // This requires 64-bit
  static_assert(sizeof(uint32_t) == 4, "bad platform");
  static_assert(sizeof(uint64_t) == 8, "bad platform");

  // val_ stores the epoch in the most significant 32 bits and the
  // waiter count in the least significant 32 bits.
  std::atomic<uint64_t> val_;

  static constexpr uint64_t kAddWaiter = uint64_t(1);

  static constexpr size_t kEpochShift = 32;
  static constexpr uint64_t kAddEpoch = uint64_t(1) << kEpochShift;
  static constexpr uint64_t kWaiterMask = kAddEpoch - 1;
};

inline bool EventCount::notify() noexcept {
  uint64_t prev = val_.fetch_add(kAddEpoch, std::memory_order_release);

  if (ABSL_PREDICT_FALSE(prev & kWaiterMask)) {
    auto* active_ctx = ::boost::fibers::context::active();

    /*
    lk makes sure that when a waiting thread is entered the critical section in
    EventCount::wait, it atomically checks val_ when entering the WAIT state.
    We need it in order to make sure that cnd_.notify() is not called before the waiting
    thread enters WAIT state and thus the notification is missed.
    */
    spinlock_lock_t lk{wait_queue_splk_};
    while (!wait_queue_.empty()) {
      auto* ctx = &wait_queue_.front();
      wait_queue_.pop_front();

      if (should_switch(ctx, reinterpret_cast<std::intptr_t>(this))) {
        // notify context
        lk.unlock();
        active_ctx->schedule(ctx);
        break;
      }
    }

    return true;
  }
  return false;
}

inline bool EventCount::notifyAll() noexcept {
  uint64_t prev = val_.fetch_add(kAddEpoch, std::memory_order_release);

  if (ABSL_PREDICT_FALSE(prev & kWaiterMask)) {
    auto* active_ctx = ::boost::fibers::context::active();

    spinlock_lock_t lk{wait_queue_splk_};
    wait_queue_t tmp;
    tmp.swap(wait_queue_);
    lk.unlock();

    while (!tmp.empty()) {
      ::boost::fibers::context* ctx = &tmp.front();
      tmp.pop_front();

      if (should_switch(ctx, reinterpret_cast<std::intptr_t>(this))) {
        // notify context
        active_ctx->schedule(ctx);
      }
    }
  }

  return false;
};

// Atomically checks for epoch and waits on cond_var.
inline void EventCount::wait(uint32_t epoch) noexcept {
  auto* active_ctx = ::boost::fibers::context::active();

  spinlock_lock_t lk{wait_queue_splk_};
  if ((val_.load(std::memory_order_relaxed) >> kEpochShift) == epoch) {
    // atomically call lt.unlock() and block on *this
    // store this fiber in waiting-queue
    active_ctx->wait_link(wait_queue_);
    active_ctx->twstatus.store(static_cast<std::intptr_t>(0), std::memory_order_release);

    // suspend this fiber
    active_ctx->suspend(lk);
  }
}

// Returns true if had to preempt, false if no preemption happenned.
template <typename Condition> bool EventCount::await(Condition condition) {
  if (condition())
    return false;  // fast path

  // condition() is the only thing that may throw, everything else is
  // noexcept, Key destructor makes sure to cancelWait state when exiting the function.
  bool preempt = false;
  while (true) {
    Key key = prepareWait(); // Key destructor restores back the sequence counter.
    if (condition()) {
      break;
    }
    preempt = true;
    wait(key.epoch());
  }
  return preempt;
}

}  // namespace fibers_ext
}  // namespace util
