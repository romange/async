// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <liburing.h>
#include <pthread.h>

#include "util/proactor_base.h"
#include "util/uring/submit_entry.h"

namespace util {
namespace uring {

class UringFiberAlgo;

class Proactor : public ProactorBase {
  Proactor(const Proactor&) = delete;
  void operator=(const Proactor&) = delete;

 public:
  Proactor();
  ~Proactor();

  void Init(size_t ring_size, int wq_fd = -1);

  // Runs the poll-loop. Stalls the calling thread which will become the "Proactor" thread.
  void Run() final;

  using IoResult = int;

  // IoResult is the I/O result of the completion event.
  // uint32_t - epoll flags.
  // int64_t is the payload supplied during event submission. See GetSubmitEntry below.
  using CbType = std::function<void(IoResult, uint32_t, int64_t, Proactor*)>;

  /**
   * @brief Get the Submit Entry object in order to issue I/O request.
   *
   * @param cb - completion callback.
   * @param payload - an argument to the completion callback that is further passed as the second
   *                  argument to CbType(). Can be nullptr if no notification is required.
   * @return SubmitEntry with initialized userdata.
   *
   * This method might block the calling fiber therefore it should not be called within proactor
   * context. In other words it can not be called from  *Brief([]...) calls to Proactor.
   * In addition, this method can not be used for introducing IOSQE_IO_LINK chains since they
   * require atomic SQE allocation.
   * @todo We should add GetSubmitEntries that can allocate multiple SQEs atomically.
   *       In that case we will need RegisterCallback function that takes an unregistered SQE
   *       and assigns a callback to it. GetSubmitEntry will be implemented using those functions.
   */
  SubmitEntry GetSubmitEntry(CbType cb, int64_t payload);


  // Uring configuration options.
  bool HasFastPoll() const {
    return fast_poll_f_;
  }

  bool HasSqPoll() const {
    return sqpoll_f_;
  }

  bool HasRegisterFd() const {
    return register_fd_;
  }

  int ring_fd() const {
    return ring_.ring_fd;
  }

  unsigned RegisterFd(int source_fd);

  int TranslateFixedFd(int fixed_fd) const {
    return register_fd_ && fixed_fd >= 0 ? register_fds_[fixed_fd] : fixed_fd;
  }

  void UnregisterFd(unsigned fixed_fd);
  LinuxSocketBase* CreateSocket() final;

 private:
  void DispatchCompletions(io_uring_cqe* cqes, unsigned count);
  void CheckForTimeoutSupport();

  void RegrowCentries();
  void ArmWakeupEvent();
  void SchedulePeriodic(uint32_t id, std::shared_ptr<PeriodicItem> item) final;
  void PeriodicCb(IoResult res, int64_t task_id, std::shared_ptr<PeriodicItem> item);
  void CancelPeriodicInternal(std::shared_ptr<PeriodicItem> item) final;

  io_uring ring_;

  int  wake_fixed_fd_;

  uint8_t fast_poll_f_ : 1;
  uint8_t sqpoll_f_ : 1;
  uint8_t register_fd_ : 1;
  uint8_t support_timeout_ : 1;
  uint8_t reserved_f_ : 4;

  EventCount sqe_avail_;
  ::boost::fibers::context* main_loop_ctx_ = nullptr;

  friend class UringFiberAlgo;

  struct CompletionEntry {
    CbType cb;

    // serves for linked list management when unused. Also can store an additional payload
    // field when in flight.
    int32_t val = -1;
    int32_t opcode = -1;  // For debugging. TODO: to remove later.
  };
  static_assert(sizeof(CompletionEntry) == 40, "");

  std::vector<CompletionEntry> centries_;
  std::vector<int> register_fds_;
  int32_t next_free_ce_ = -1;
  uint32_t next_free_fd_ = 0;
};

}  // namespace uring
}  // namespace util
