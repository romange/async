// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/epoll/ev_pool.h"

#include "base/logging.h"
#include "base/pthread_utils.h"

DEFINE_uint32(evpool_threads, 0, "Number of io threads in the pool");

using namespace std;

namespace util {
namespace epoll {

EvPool::EvPool(std::size_t pool_size) {
  if (pool_size == 0) {
    pool_size = FLAGS_evpool_threads > 0 ? FLAGS_evpool_threads
                                           : thread::hardware_concurrency();
  }
  pool_size_ = pool_size;
  ev_cntrl_.reset(new EvController[pool_size]);
}

EvPool::~EvPool() {
  Stop();
}

void EvPool::CheckRunningState() {
  CHECK_EQ(RUN, state_);
}

void EvPool::Run() {
  CHECK_EQ(STOPPED, state_);

  char buf[32];

  auto init_proactor = [this, &buf](int i) mutable {
    snprintf(buf, sizeof(buf), "EvController%u", i);
    auto cb = [ptr = &ev_cntrl_[i]]() { ptr->Run(); };
    pthread_t tid = base::StartThread(buf, cb);
    cpu_set_t cps;
    CPU_ZERO(&cps);
    CPU_SET(i % thread::hardware_concurrency(), &cps);

    int rc = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cps);
    LOG_IF(WARNING, rc) << "Error calling pthread_setaffinity_np: "
                        << strerror(rc) << "\n";
  };

  for (size_t i = 0; i < pool_size_; ++i) {
    init_proactor(i);
  }
  state_ = RUN;

  for (unsigned i = 0; i < size(); ++i) {
    EvController& cntrl = ev_cntrl_[i];
    // func must be copied, it can not be moved, because we dsitribute it into
    // multiple EvControllers.
    cntrl.AsyncBrief([i]()  { EvController::SetIndex(i); });
  }

  AwaitOnAll([](EvController*) {});  // Barrier to wait for all ev-loops to start spinning.

  LOG(INFO) << "Running " << pool_size_ << " io threads";
}

void EvPool::Stop() {
  if (state_ == STOPPED)
    return;

  for (size_t i = 0; i < pool_size_; ++i) {
    ev_cntrl_[i].Stop();
  }

  VLOG(1) << "EvControllers have been stopped";

  for (size_t i = 0; i < pool_size_; ++i) {
    pthread_join(ev_cntrl_[i].thread_id(), nullptr);
    VLOG(2) << "Thread " << i << " has joined";
  }
  state_ = STOPPED;
}

EvController* EvPool::GetNextController() {
  uint32_t index = next_io_context_.load(std::memory_order_relaxed);
  // Use a round-robin scheme to choose the next io_context to use.
  DCHECK_LT(index, pool_size_);

  EvController& proactor = at(index++);

  // Not-perfect round-robind since this function is non-transactional but it "works".
  if (index >= pool_size_)
    index = 0;

  next_io_context_.store(index, std::memory_order_relaxed);
  return &proactor;
}

absl::string_view EvPool::GetString(absl::string_view source) {
  if (source.empty()) {
    return source;
  }

  folly::RWSpinLock::ReadHolder rh(str_lock_);
  auto it = str_set_.find(source);
  if (it != str_set_.end())
    return *it;
  rh.reset();

  str_lock_.lock();
  char* str = arena_.Allocate(source.size());
  memcpy(str, source.data(), source.size());

  absl::string_view res(str, source.size());
  str_set_.insert(res);
  str_lock_.unlock();

  return res;
}

}  // namespace epoll
}  // namespace util
