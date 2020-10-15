// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/listener_interface.h"

#include <signal.h>

#include <boost/fiber/operations.hpp>

#include "base/logging.h"
#include "util/fiber_sched_algo.h"
#include "util/proactor_pool.h"
#include "util/uring/accept_server.h"

#define VSOCK(verbosity, sock) VLOG(verbosity) << "sock[" << (sock).native_handle() << "] "
#define DVSOCK(verbosity, sock) DVLOG(verbosity) << "sock[" << (sock).native_handle() << "] "

namespace util {

using namespace boost;
using namespace std;

using ListType =
    intrusive::slist<Connection, Connection::member_hook_t, intrusive::constant_time_size<true>,
                     intrusive::cache_last<false>>;

struct ListenerInterface::SafeConnList {
  ListType list;
  fibers::mutex mu;
  fibers::condition_variable cond;

  void Link(Connection* c) {
    std::lock_guard<fibers::mutex> lk(mu);
    list.push_front(*c);
    VLOG(2) << "List size " << list.size();
  }

  void Unlink(Connection* c) {
    std::lock_guard<fibers::mutex> lk(mu);
    auto it = list.iterator_to(*c);
    list.erase(it);
    DVLOG(2) << "List size " << list.size();

    if (list.empty()) {
      cond.notify_one();
    }
  }

  void AwaitEmpty() {
    std::unique_lock<fibers::mutex> lk(mu);
    DVLOG(1) << "AwaitEmpty: List size: " << list.size();

    cond.wait(lk, [this] { return list.empty(); });
  }
};

// Runs in a dedicated fiber for each listener.
void ListenerInterface::RunAcceptLoop() {
  auto& fiber_props = this_fiber::properties<FiberProps>();
  fiber_props.set_name("AcceptLoop");

  auto ep = sock_->LocalEndpoint();
  VSOCK(0, *sock_) << "AcceptServer - listening on port " << ep.port();
  SafeConnList safe_list;

  PreAcceptLoop(sock_->proactor());

  while (true) {
    FiberSocketBase::accept_result res = sock_->Accept();
    if (!res.has_value()) {
      FiberSocketBase::error_code ec = res.error();
      if (ec != errc::connection_aborted) {
        LOG(ERROR) << "Error calling accept " << ec << "/" << ec.message();
      }
      break;
    }
    std::unique_ptr<FiberSocketBase> peer{res.value()};

    VLOG(2) << "Accepted " << peer->native_handle() << ": " << peer->LocalEndpoint();
    ProactorBase* next = pool_->GetNextProactor();  // Could be for another thread.
    peer->SetProactor(next);
    Connection* conn = NewConnection(next);
    conn->SetSocket(peer.release());
    safe_list.Link(conn);

    // mutable because we move peer.
    auto cb = [conn, next, &safe_list]() mutable {
      next->AsyncFiber(&RunSingleConnection, conn, &safe_list);
    };

    // Run cb in its Proactor thread.
    next->AsyncFiber(std::move(cb));
  }

  PreShutdown();

  safe_list.mu.lock();
  unsigned cnt = 0;
  for (auto& val : safe_list.list) {
    val.Shutdown();
    DVSOCK(1, *val.socket_) << "Shutdown";
    ++cnt;
  }

  safe_list.mu.unlock();

  VLOG(1) << "Waiting for " << cnt << " connections to close";
  safe_list.AwaitEmpty();

  PostShutdown();

  LOG(INFO) << "Listener stopped for port " << ep.port();
}

ListenerInterface::~ListenerInterface() {
  VLOG(1) << "Destroying ListenerInterface " << this;
}

void ListenerInterface::RunSingleConnection(Connection* conn, SafeConnList* conns) {
  VSOCK(2, *conn) << "Running connection";

  std::unique_ptr<Connection> guard(conn);
  try {
    conn->HandleRequests();
    VSOCK(2, *conn) << "After HandleRequests";

  } catch (std::exception& e) {
    LOG(ERROR) << "Uncaught exception " << e.what();
  }
  conns->Unlink(conn);
}

void ListenerInterface::RegisterPool(ProactorPool* pool) {
  // In tests we might relaunch AcceptServer with the same listener, so we allow
  // reassigning the same pool.
  CHECK(pool_ == nullptr || pool_ == pool);

  pool_ = pool;
}

}  // namespace util
