// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

// for tcp::endpoint. Consider introducing our own.
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/fiber/context.hpp>

#include "absl/base/attributes.h"
#include "util/fiber_socket_base.h"
#include "util/epoll/ev_controller.h"

namespace util {
namespace epoll {

class FiberSocket : public FiberSocketBase {
 public:

  FiberSocket(EvController* ev) : FiberSocketBase(-1, ev) {
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
  EvController* GetEv() { return static_cast<EvController*>(proactor()); }
  void OnSetProactor() final;

  void Wakey(uint32_t mask, EvController* cntr);

  ::boost::fibers::context* current_context_ = nullptr;
  int arm_index_ = -1;
};

}  // namespace epoll
}  // namespace util
