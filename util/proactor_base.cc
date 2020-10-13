// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/proactor_base.h"

#include <sys/eventfd.h>
#include "base/logging.h"

using namespace boost;
namespace ctx = boost::context;


namespace util {

thread_local ProactorBase::TLInfo ProactorBase::tl_info_;

ProactorBase::ProactorBase() : task_queue_(512) {
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  CHECK_GE(wake_fd_, 0);
  VLOG(1) << "Created wake_fd is " << wake_fd_;

  volatile ctx::fiber dummy;  // For some weird reason I need this to pull
                              // boost::context into linkage.
}

ProactorBase::~ProactorBase() {
  close(wake_fd_);
}

uint64_t ProactorBase::AddIdleTask(IdleTask f) {
  uint64_t id = next_idle_task_++;
  auto res = idle_map_.emplace(id, std::move(f));
  CHECK(res.second);
  return id;
}

}  // namespace util
