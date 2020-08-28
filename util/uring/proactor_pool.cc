// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/proactor_pool.h"

#include "base/logging.h"
#include "base/pthread_utils.h"

DEFINE_uint32(proactor_threads, 0, "Number of io threads in the pool");

// TODO: To fix proactor_reuse_wq
DEFINE_bool(proactor_reuse_wq, false, "If true reuses the same work-queue for all io_urings "
                                     "in the pool");

using namespace std;

namespace util {
namespace uring {

ProactorPool::ProactorPool(std::size_t pool_size) {
  if (pool_size == 0) {
    pool_size = FLAGS_proactor_threads > 0 ? FLAGS_proactor_threads
                                           : thread::hardware_concurrency();
  }
  pool_size_ = pool_size;
  proactor_.reset(new Proactor[pool_size]);
}

ProactorPool::~ProactorPool() {
  Stop();
}

void ProactorPool::CheckRunningState() {
  CHECK_EQ(RUN, state_);
}

void ProactorPool::Run(uint32_t ring_depth) {
  CHECK_EQ(STOPPED, state_);

  char buf[32];

  auto init_proactor = [this, ring_depth, &buf](int i, int wq_fd) mutable {
    snprintf(buf, sizeof(buf), "Proactor%u", i);
    auto cb = [ptr = &proactor_[i], wq_fd, ring_depth]() { ptr->Run(ring_depth, wq_fd); };
    pthread_t tid = base::StartThread(buf, cb);
    cpu_set_t cps;
    CPU_ZERO(&cps);
    CPU_SET(i % thread::hardware_concurrency(), &cps);

    int rc = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cps);
    LOG_IF(WARNING, rc) << "Error calling pthread_setaffinity_np: "
                        << strerror(rc) << "\n";
  };
  init_proactor(0, -1);
  int wq_fd = FLAGS_proactor_reuse_wq ? proactor_[0].ring_fd() : -1;

  for (size_t i = 1; i < pool_size_; ++i) {
    init_proactor(i, wq_fd);
  }
  state_ = RUN;

  AwaitOnAll([](unsigned index, Proactor*) {
    Proactor::SetIndex(index);
  });

  LOG(INFO) << "Running " << pool_size_ << " io threads";
}

void ProactorPool::Stop() {
  if (state_ == STOPPED)
    return;

  for (size_t i = 0; i < pool_size_; ++i) {
    proactor_[i].Stop();
  }

  VLOG(1) << "Proactors have been stopped";

  for (size_t i = 0; i < pool_size_; ++i) {
    pthread_join(proactor_[i].thread_id(), nullptr);
    VLOG(2) << "Thread " << i << " has joined";
  }
  state_ = STOPPED;
}

Proactor* ProactorPool::GetNextProactor() {
  uint32_t index = next_io_context_.load(std::memory_order_relaxed);
  // Use a round-robin scheme to choose the next io_context to use.
  DCHECK_LT(index, pool_size_);

  Proactor& proactor = at(index++);

  // Not-perfect round-robind since this function is non-transactional but it "works".
  if (index >= pool_size_)
    index = 0;

  next_io_context_.store(index, std::memory_order_relaxed);
  return &proactor;
}

absl::string_view ProactorPool::GetString(absl::string_view source) {
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

}  // namespace uring
}  // namespace util
