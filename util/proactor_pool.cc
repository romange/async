// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/proactor_pool.h"

#include "base/logging.h"
#include "base/pthread_utils.h"
// #include "util/uring/proactor.h"

DEFINE_uint32(proactor_threads, 0, "Number of io threads in the pool");

using namespace std;

namespace util {

ProactorPool::ProactorPool(std::size_t pool_size) {
  if (pool_size == 0) {
    pool_size = FLAGS_proactor_threads > 0 ? FLAGS_proactor_threads
                                           : thread::hardware_concurrency();
  }
  pool_size_ = pool_size;
  proactor_.reset(new ProactorBase*[pool_size]);
  std::fill(proactor_.get(), proactor_.get() + pool_size, nullptr);
}

ProactorPool::~ProactorPool() {
  Stop();
  for (size_t i = 0; i < pool_size_; ++i) {
    delete proactor_[i];
  }
}

void ProactorPool::CheckRunningState() {
  CHECK_EQ(RUN, state_);
}

void ProactorPool::Run() {
  SetupProactors();

  AwaitOnAll([](unsigned index, auto*) {
    // It seems to simplify things in kernel for io_uring.
    // https://github.com/axboe/liburing/issues/218
    // I am not sure what's how it impacts higher application levels.
    unshare(CLONE_FS);
    ProactorBase::SetIndex(index);
  });

  LOG(INFO) << "Running " << pool_size_ << " io threads";
}

void ProactorPool::Stop() {
  if (state_ == STOPPED)
    return;

  for (size_t i = 0; i < pool_size_; ++i) {
    proactor_[i]->Stop();
  }

  VLOG(1) << "Proactors have been stopped";

  for (size_t i = 0; i < pool_size_; ++i) {
    pthread_join(proactor_[i]->thread_id(), nullptr);
    VLOG(2) << "Thread " << i << " has joined";
  }
  state_ = STOPPED;
}

ProactorBase* ProactorPool::GetNextProactor() {
  uint32_t index = next_io_context_.load(std::memory_order_relaxed);
  // Use a round-robin scheme to choose the next io_context to use.
  DCHECK_LT(index, pool_size_);

  ProactorBase* proactor = at(index++);

  // Not-perfect round-robind since this function is non-transactional but it "works".
  if (index >= pool_size_)
    index = 0;

  next_io_context_.store(index, std::memory_order_relaxed);
  return proactor;
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

void ProactorPool::SetupProactors() {
  CHECK_EQ(STOPPED, state_);

  char buf[32];

  for (unsigned i = 0; i < pool_size_; ++i) {
    snprintf(buf, sizeof(buf), "Proactor%u", i);

    proactor_[i]= CreateProactor();
    auto cb = [this, i]() mutable {
      this->InitInThread(i);
      proactor_[i]->Run();
    };

    pthread_t tid = base::StartThread(buf, std::move(cb));
    cpu_set_t cps;
    CPU_ZERO(&cps);
    CPU_SET(i % thread::hardware_concurrency(), &cps);

    int rc = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cps);
    LOG_IF(WARNING, rc) << "Error calling pthread_setaffinity_np: "
                        << strerror(rc) << "\n";
  }

  state_ = RUN;
}

}  // namespace util
