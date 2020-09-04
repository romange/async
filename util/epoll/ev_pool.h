// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <pthread.h>

#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "base/arena.h"
#include "base/type_traits.h"
#include "base/RWSpinLock.h"
#include "util/epoll/ev_controller.h"

namespace util {
namespace epoll {

class EvPool {
  template <typename Func, typename... Args>
  using AcceptArgsCheck =
      typename std::enable_if<base::is_invocable<Func, Args...>::value,
                              int>::type;

 public:
  EvPool(const EvPool&) = delete;
  void operator=(const EvPool&) = delete;

  //! Constructs io_context pool with number of threads equal to 'pool_size'.
  //! pool_size = 0 chooses automatically pool size equal to number of cores in
  //! the system.
  explicit EvPool(std::size_t pool_size = 0);

  ~EvPool();

  //! Starts running all Proactor objects in the pool.
  //! Blocks until all the controllers up and spinning.
  void Run();

  /*! @brief Stops all io_context objects in the pool.
   *
   *  Waits for all the threads to finish. Requires that Run has been called.
   *  Blocks the current thread until all the pool threads exited.
   */
  void Stop();

  //! Get a Proactor to use. Thread-safe.
  EvController* GetNextController();

  EvController& operator[](size_t i) {
    return at(i);
  }

  EvController& at(size_t i) {
    return ev_cntrl_[i];
  }

  size_t size() const {
    return pool_size_;
  }

  /*! @brief Runs func in all IO threads asynchronously.
   *
   * The task must be CPU-only non IO-blocking code because it runs directly in
   * IO-fiber. AsyncOnAll runs asynchronously and will exit before  the task
   * finishes. The 'func' must accept EvController& as its argument.
   */
  template <typename Func, AcceptArgsCheck<Func, EvController*> = 0>
  void AsyncOnAll(Func&& func) {
    CheckRunningState();
    for (unsigned i = 0; i < size(); ++i) {
      EvController& context = ev_cntrl_[i];
      // func must be copied, it can not be moved, because we dsitribute it into
      // multiple EvControllers.
      context.AsyncBrief([&context, func]() mutable { func(&context); });
    }
  }

  /**
   * @brief Runs the funcion in all IO threads asynchronously.
   * Blocks until all the asynchronous calls return.
   *
   * Func must accept "EvController&" and it should not block.
   */
  template <typename Func, AcceptArgsCheck<Func, EvController*> = 0>
  void AwaitOnAll(Func&& func) {
    fibers_ext::BlockingCounter bc(size());
    auto cb = [func = std::forward<Func>(func), bc](EvController* context) mutable {
      func(context);
      bc.Dec();
    };
    AsyncOnAll(std::move(cb));
    bc.Wait();
  }

  /**
   * @brief Runs `func` in a fiber asynchronously. func must accept Proactor&.
   *        func may fiber-block.
   *
   * @param func
   *
   * 'func' callback runs inside a wrapping fiber.
   */
  template <typename Func, AcceptArgsCheck<Func,  EvController*> = 0>
  void AsyncFiberOnAll(Func&& func) {
    AsyncOnAll(
        [func = std::forward<Func>(func)](EvController* context) {
          ::boost::fibers::fiber(func, context).detach();
        });
  }


  /**
   * @brief Runs `func` wrapped in fiber on all IO threads in parallel. func
   * must accept Proactor&. func may fiber-block.
   *
   * @param func
   *
   * Waits for all the callbacks to finish.
   */
  template <typename Func, AcceptArgsCheck<Func, EvController*> = 0>
  void AwaitFiberOnAll(Func&& func) {
    fibers_ext::BlockingCounter bc(size());
    auto cb = [func = std::forward<Func>(func), bc](EvController* context) mutable {
      func(context);
      bc.Dec();
    };
    AsyncFiberOnAll(std::move(cb));
    bc.Wait();
  }

  EvController* GetLocalController();

  // Auxillary functions

  // Returns a string owned by pool's global storage. Allocates only once for each new string blob.
  // Currently has average performance and it employs RW spinlock underneath.
  absl::string_view GetString(absl::string_view source);

 private:
  void WrapLoop(size_t index, fibers_ext::BlockingCounter* bc);
  void CheckRunningState();

  std::unique_ptr<EvController[]> ev_cntrl_;

  /// The next io_context to use for a connection.
  std::atomic_uint_fast32_t next_io_context_{0};
  uint32_t pool_size_;

  folly::RWSpinLock str_lock_;
  absl::flat_hash_set<absl::string_view> str_set_;
  base::Arena arena_;

  enum State { STOPPED, RUN } state_ = STOPPED;
};


}  // namespace epoll
}  // namespace util
