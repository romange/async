// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <liburing/io_uring.h>

namespace util {
namespace uring {

class Proactor;

class SubmitEntry {
  io_uring_sqe* sqe_;

 public:
  SubmitEntry() : sqe_(nullptr) {
  }

  // mask is a bit-OR of POLLXXX flags.
  void PrepPollAdd(int fd, short mask) {
    PrepFd(IORING_OP_POLL_ADD, fd);
    sqe_->poll_events = mask;
  }

  void PrepRecvMsg(int fd, const struct msghdr* msg, unsigned flags) {
    PrepFd(IORING_OP_RECVMSG, fd);
    sqe_->addr = (unsigned long)msg;
    sqe_->len = 1;
    sqe_->msg_flags = flags;
  }

  void PrepRead(int fd, void* buf, unsigned size, size_t offset) {
    PrepFd(IORING_OP_READ, fd);
    sqe_->addr = (unsigned long)buf;
    sqe_->len = size;
    sqe_->off = offset;
  }

  void PrepSendMsg(int fd, const struct msghdr* msg, unsigned flags) {
    PrepFd(IORING_OP_SENDMSG, fd);
    sqe_->addr = (unsigned long)msg;
    sqe_->len = 1;
    sqe_->msg_flags = flags;
  }

  void PrepConnect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    PrepFd(IORING_OP_CONNECT, fd);
    sqe_->addr = (unsigned long)addr;
    sqe_->len = 0;
    sqe_->off = addrlen;
  }

  void PrepTimeout(const timespec* ts, bool is_abs = true) {
    PrepFd(IORING_OP_TIMEOUT, -1);
    sqe_->addr = (unsigned long)ts;
    sqe_->len = 1;
    sqe_->timeout_flags = (is_abs ? IORING_TIMEOUT_ABS : 0);
  }

  // TODO: To remove this accessor.
  io_uring_sqe* sqe() {
    return sqe_;
  }

  void PrepNOP() {
    PrepFd(IORING_OP_NOP, -1);
  }

 private:
  explicit SubmitEntry(io_uring_sqe* sqe) : sqe_(sqe) {
  }

  void PrepFd(int op, int fd) {
    sqe_->opcode = op;
    sqe_->fd = fd;
    sqe_->flags = 0;
    sqe_->ioprio = 0;
    sqe_->addr = 0;
    sqe_->off = 0;
    sqe_->len = 0;
    sqe_->rw_flags = 0;
    sqe_->__pad2[0] = sqe_->__pad2[1] = sqe_->__pad2[2] = 0;
  }

  friend class Proactor;
};

}  // namespace uring
}  // namespace util
