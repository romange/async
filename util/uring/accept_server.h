// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <functional>
#include <vector>

#include "util/fibers/fibers_ext.h"
#include "util/uring/connection.h"
#include "util/uring/fiber_socket.h"

namespace util {
namespace uring {

class Proactor;
class ProactorPool;
class Connection;
class ListenerInterface;

class AcceptServer {
 public:
  explicit AcceptServer(ProactorPool* pool, bool break_on_int = true);
  ~AcceptServer();

  void Run();

  // If wait is false - does not wait for the server to stop.
  // Then you need to run Wait() to wait for proper shutdown.
  void Stop(bool wait = false);

  void Wait();

  // Returns the port number to which the listener was bound.
  unsigned short AddListener(unsigned short port, ListenerInterface* cf);

  void TriggerOnBreakSignal(std::function<void()> f) {
    on_break_hook_ = std::move(f);
  }

  void set_back_log(uint16_t backlog) {
    backlog_ = backlog;
  }

 private:

  void BreakListeners();

  ProactorPool* pool_;

  // Called if a termination signal has been caught (SIGTERM/SIGINT).
  std::function<void()> on_break_hook_;

  std::vector<std::unique_ptr<ListenerInterface>> list_interface_;
  fibers_ext::BlockingCounter ref_bc_;  // to synchronize listener threads during the shutdown.

  bool was_run_ = false;
  bool break_ = false;

  uint16_t backlog_ = 128;
};


/**
 * @brief Abstracts away connections implementation and their life-cycle.
 *
 */
class ListenerInterface {
 public:
  virtual ~ListenerInterface();

  void RegisterPool(ProactorPool* pool);

  //! Creates a dedicated handler for a new connection.
  //! Called per new accepted connection
  virtual Connection* NewConnection(Proactor* context) = 0;

  //! Hook to be notified when listener interface start listening and accepting sockets.
  //! Called once.
  virtual void PreAcceptLoop(Proactor* owner) {}

  // Called by AcceptServer when shutting down start and before all connections are closed.
  virtual void PreShutdown() {
  }

  // Called by AcceptServer when shutting down finalized and after all connections are closed.
  virtual void PostShutdown() {
  }

  virtual uint32_t GetSockOptMask() const { return 1 << SO_REUSEADDR; }

 protected:
  ProactorPool* pool() {
    return pool_;
  }

  FiberSocket* listener() { return &listener_;}

 private:
  struct SafeConnList;

  void RunAcceptLoop();
  static void RunSingleConnection(Connection* conn, SafeConnList* list);

  FiberSocket listener_;

  ProactorPool* pool_ = nullptr;
  friend class AcceptServer;
};

}  // namespace uring
}  // namespace util
