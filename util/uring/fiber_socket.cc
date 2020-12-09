// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/fiber_socket.h"

#include <netinet/in.h>
#include <sys/poll.h>

#include <boost/fiber/context.hpp>

#include "base/logging.h"
#include "base/stl_util.h"
#include "util/uring/proactor.h"

#define VSOCK(verbosity) VLOG(verbosity) << "sock[" << native_handle() << "] "
#define DVSOCK(verbosity) DVLOG(verbosity) << "sock[" << native_handle() << "] "

namespace util {
namespace uring {

using namespace std;
using namespace boost;
using IoResult = Proactor::IoResult;

namespace {

class FiberCall {
  SubmitEntry se_;
  fibers::context* me_;
  IoResult io_res_ = 0;
  uint32_t flags_ = 0;
 public:
  FiberCall(Proactor* proactor) : me_(fibers::context::active()) {
    register_fd_ = proactor->HasRegisterFd();

    auto waker = [this](IoResult res, uint32_t flags, int64_t, Proactor* mgr) {
      io_res_ = res;
      flags_ = flags;
      fibers::context::active()->schedule(me_);
    };
    se_ = proactor->GetSubmitEntry(std::move(waker), 0);
  }

  ~FiberCall() {
    CHECK(!me_) << "Get was not called!";
  }

  SubmitEntry* operator->() {
    return &se_;
  }

  IoResult Get() {
    se_.sqe()->flags |= (register_fd_ ? IOSQE_FIXED_FILE : 0);
    me_->suspend();
    me_ = nullptr;

    return io_res_;
  }

  uint32_t flags() const { return flags_; }
 private:
  bool register_fd_;
};

inline ssize_t posix_err_wrap(ssize_t res, FiberSocket::error_code* ec) {
  if (res == -1) {
    *ec = FiberSocket::error_code(errno, std::system_category());
  } else if (res < 0) {
    LOG(WARNING) << "Bad posix error " << res;
  }
  return res;
}

}  // namespace

FiberSocket::~FiberSocket() {
  error_code ec = Close();  // Quietly close.

  LOG_IF(WARNING, ec) << "Error closing socket " << ec << "/" << ec.message();
}

auto FiberSocket::Close() -> error_code {
  error_code ec;
  if (fd_ >= 0) {
    DVSOCK(1) << "Closing socket";

    int fd = native_handle();
    GetProactor()->UnregisterFd(fd_ & FD_MASK);
    posix_err_wrap(::close(fd), &ec);
    fd_ = -1;
  }
  return ec;
}


auto FiberSocket::Accept() -> accept_result {
  CHECK(proactor());

  sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);

  error_code ec;

  int real_fd = native_handle();
  while (true) {
    int res =
        accept4(real_fd, (struct sockaddr*)&client_addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (res >= 0) {
      FiberSocket* fs = new FiberSocket{nullptr};
      fs->fd_ = res;
      return fs;
    }

    DCHECK_EQ(-1, res);

    if (errno == EAGAIN) {
      FiberCall fc(GetProactor());
      fc->PrepPollAdd(fd_ & FD_MASK, POLLIN);
      IoResult io_res = fc.Get();

      if (io_res == POLLERR) {
        ec = system::errc::make_error_code(system::errc::connection_aborted);
        return nonstd::make_unexpected(ec);
      }
      continue;
    }

    posix_err_wrap(res, &ec);
    return nonstd::make_unexpected(ec);
  }
}

auto FiberSocket::Connect(const endpoint_type& ep) -> error_code {
  CHECK_EQ(fd_, -1);
  CHECK(proactor() && proactor()->InMyThread());

  error_code ec;

  fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (posix_err_wrap(fd_, &ec) < 0)
    return ec;

  Proactor* p = GetProactor();
  if (p->HasSqPoll()) {
    LOG(FATAL) << "Not supported with SQPOLL, TBD";
  }
  unsigned dense_id = p->RegisterFd(fd_);
  IoResult io_res;

  if (p->HasFastPoll()) {
    FiberCall fc(p);
    fc->PrepConnect(dense_id, ep.data(), ep.size());
    io_res = fc.Get();
  } else {
    int res = connect(fd_, ep.data(), ep.size());
    if (res == 0) {
      return ec;
    }

    if (errno != EINPROGRESS) {
      return error_code{errno, system::system_category()};
    }

    FiberCall fc(p);
    fc->PrepPollAdd(dense_id, POLLOUT | POLLIN | POLLERR);
    io_res = fc.Get();
  }

  if (io_res < 0) {  // In that case connect returns -errno.
    if (close(fd_) < 0) {
      LOG(WARNING) << "Could not close fd " << strerror(errno);
    }
    fd_ = -1;
    ec = error_code(-io_res, system::system_category());
  } else {
    // Not sure if this check is needed, to be on the safe side.
    int serr = 0;
    socklen_t slen = sizeof(serr);
    CHECK_EQ(0, getsockopt(fd_, SOL_SOCKET, SO_ERROR, &serr, &slen));
    CHECK_EQ(0, serr);
  }
  return ec;
}

auto FiberSocket::Send(const iovec* ptr, size_t len) -> expected_size_t {
  CHECK(proactor());
  CHECK_GT(len, 0U);
  CHECK_GE(fd_, 0);

  if (fd_ & IS_SHUTDOWN) {
    return nonstd::make_unexpected(std::make_error_code(std::errc::connection_aborted));
  }

  msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = const_cast<iovec*>(ptr);
  msg.msg_iovlen = len;

  ssize_t res;
  int fd = fd_ & FD_MASK;
  Proactor* p = GetProactor();
  while (true) {
    FiberCall fc(p);
    fc->PrepSendMsg(fd, &msg, MSG_NOSIGNAL);
    res = fc.Get();  // Interrupt point
    if (res >= 0) {
      return res;  // Fastpath
    }
    DVSOCK(1) << "Got " << res;
    res = -res;
    if (res == EAGAIN)  // EAGAIN can happen in case of CQ overflow.
      continue;

    if (base::_in(res, {ECONNABORTED, EPIPE, ECONNRESET, ECANCELED})) {
      if (res == EPIPE)  // We do not care about EPIPE that can happen when we shutdown our socket.
        res = ECONNABORTED;
      break;
    }

    LOG(FATAL) << "Unexpected error " << res << "/" << strerror(res);
  }
  std::error_code ec(res, std::generic_category());
  VSOCK(1) << "Error " << ec << " on " << RemoteEndpoint();

  return nonstd::make_unexpected(std::move(ec));
}

auto FiberSocket::RecvMsg(const msghdr& msg, int flags) -> expected_size_t {
  CHECK(proactor());
  CHECK_GE(fd_, 0);

  if (fd_ & IS_SHUTDOWN) {
    return nonstd::make_unexpected(std::make_error_code(std::errc::connection_aborted));
  }
  int fd = fd_ & FD_MASK;
  Proactor* p = GetProactor();

  // There is a possible data-race bug since GetSubmitEntry can preempt inside
  // FiberCall, thus introducing a chain with random SQE not from here.
  //
  // The bug is not really interesting in this context here since we handle the use-case of old
  // kernels without fast-poll, however it's problematic for transactions that require SQE chains.
  // Added TODO to proactor.h
  if (!p->HasFastPoll()) {
    DVSOCK(1) << "POLLIN";
    auto cb = [this](IoResult res, uint32_t, int32_t, Proactor* mgr) {
      DVSOCK(1) << "POLLING RES " << res;
    };
    SubmitEntry se = p->GetSubmitEntry(std::move(cb), 0);
    se.PrepPollAdd(fd, POLLIN);
    se.sqe()->flags |= IOSQE_IO_LINK;
  }

  ssize_t res;
  while (true) {
    FiberCall fc(p);
    fc->PrepRecvMsg(fd, &msg, flags);
    res = fc.Get();

    if (res > 0) {
      return res;
    }
    DVSOCK(1) << "Got " << res;

    res = -res;
    if (res == EAGAIN) // EAGAIN can happen in case of CQ overflow.
      continue;

    if (res == 0)
      res = ECONNABORTED;

    if (base::_in(res, {ECONNABORTED, EPIPE, ECONNRESET, ECANCELED})) {
      break;
    }

    LOG(FATAL) << "sock[" << fd << "] Unexpected error " << res << "/" << strerror(res);
  }
  std::error_code ec(res, std::system_category());
  VSOCK(1) << "Error " << ec << " on " << RemoteEndpoint();

  return nonstd::make_unexpected(std::move(ec));
}

}  // namespace uring
}  // namespace util
