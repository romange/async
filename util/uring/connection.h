// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#pragma once

#include <boost/intrusive/slist.hpp>
#include <functional>

#include "util/fiber_socket_base.h"

namespace util {

namespace uring {
class ListenerInterface;
} //

class Connection {
  using connection_hook_t = ::boost::intrusive::slist_member_hook<
      ::boost::intrusive::link_mode<::boost::intrusive::normal_link>>;
  connection_hook_t hook_;


  void SetSocket(FiberSocketBase* s) { socket_.reset(s); }

  auto native_handle() const { return socket_->native_handle(); }

 public:
  using member_hook_t =
      ::boost::intrusive::member_hook<Connection, connection_hook_t, &Connection::hook_>;

  virtual ~Connection() {}

 protected:

  // The main loop for a connection. Runs in the same proactor thread as of socket_.
  virtual void HandleRequests() = 0;

  std::unique_ptr<FiberSocketBase> socket_;
  friend class uring::ListenerInterface;
};

}  // namespace util
