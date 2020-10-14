// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <liburing/io_uring.h>

#include "util/fiber_socket_base.h"

namespace util {
namespace uring {

class Proactor;

class FiberSocket : public FiberSocketBase {
  FiberSocket(int fd, Proactor* p) : FiberSocketBase(fd), proactor_(p) {}

 public:
  FiberSocket(Proactor* p = nullptr) : FiberSocket(-1, p) {
  }

  virtual ~FiberSocket();

  ABSL_MUST_USE_RESULT accept_result Accept() final;

  ABSL_MUST_USE_RESULT error_code Connect(const endpoint_type& ep) final;
  ABSL_MUST_USE_RESULT error_code Close() final;

  // Really need here expected.
  expected_size_t Send(const iovec* ptr, size_t len) override;

  expected_size_t RecvMsg(const msghdr& msg, int flags);

  void SetProactor(Proactor* p);

  Proactor* proactor() { return proactor_; }

  using FiberSocketBase::IsConnClosed;

 private:

  // We must reference proactor in each socket so that we could support write_some/read_some
  // with predefined interfance and be compliant with SyncWriteStream/SyncReadStream concepts.
  Proactor* proactor_;
};

}  // namespace uring
}  // namespace util
