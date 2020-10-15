// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <liburing/io_uring.h>

#include "util/fiber_socket_base.h"
#include "util/uring/proactor.h"

namespace util {
namespace uring {

class FiberSocket : public FiberSocketBase {
 public:
  FiberSocket(Proactor* p = nullptr) : FiberSocketBase(-1, p) {
  }

  virtual ~FiberSocket();

  ABSL_MUST_USE_RESULT accept_result Accept() final;

  ABSL_MUST_USE_RESULT error_code Connect(const endpoint_type& ep) final;
  ABSL_MUST_USE_RESULT error_code Close() final;

  // Really need here expected.
  expected_size_t Send(const iovec* ptr, size_t len) override;

  expected_size_t RecvMsg(const msghdr& msg, int flags);

  using FiberSocketBase::IsConnClosed;

 private:
  Proactor* GetProactor() { return static_cast<Proactor*>(proactor()); }
};

}  // namespace uring
}  // namespace util
