// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/epoll/fiber_socket.h"

#include <netinet/in.h>
#include <sys/epoll.h>

#include "base/logging.h"
#include "base/stl_util.h"
#include "util/epoll/ev_controller.h"

#define VSOCK(verbosity) VLOG(verbosity) << "sock[" << native_handle() << "] "
#define DVSOCK(verbosity) DVLOG(verbosity) << "sock[" << native_handle() << "] "

namespace util {
namespace epoll {

using namespace std;
using namespace boost;

namespace {


inline FiberSocket::error_code from_errno() {
  return FiberSocket::error_code(errno, std::system_category());
}

inline ssize_t posix_err_wrap(ssize_t res, FiberSocket::error_code* ec) {
  if (res == -1) {
    *ec = from_errno();
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

FiberSocket& FiberSocket::operator=(FiberSocket&& other) noexcept {
  if (fd_ >= 0) {
    error_code ec = Close();
    LOG_IF(WARNING, ec) << "Error closing socket " << ec << "/" << ec.message();
  }
  DCHECK_EQ(-1, fd_);

  swap(fd_, other.fd_);
  p_ = other.p_;
  other.p_ = nullptr;

  return *this;
}

auto FiberSocket::Shutdown(int how) -> error_code {
  CHECK_GE(fd_, 0);

  // If we shutdown and then try to Send/Recv - the call will stall since no data
  // is sent/received. Therefore we remember the state to allow consistent API experience.
  error_code ec;
  if (fd_ & IS_SHUTDOWN)
    return ec;

  posix_err_wrap(::shutdown(fd_, how), &ec);
  fd_ |= IS_SHUTDOWN;  // Enter shutdown state unrelated to the success of the call.

  return ec;
}

auto FiberSocket::Close() -> error_code {
  error_code ec;
  if (fd_ >= 0) {
    DVSOCK(1) << "Closing socket";

    int fd = fd_ & FD_MASK;
    p_->Disarm(arm_index_);
    posix_err_wrap(::close(fd), &ec);
    fd_ = -1;
  }
  return ec;
}

auto FiberSocket::Listen(unsigned port, unsigned backlog, uint32_t sock_opts_mask) -> error_code {
  CHECK_EQ(fd_, -1) << "Close socket before!";

  error_code ec;
  fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (posix_err_wrap(fd_, &ec) < 0)
    return ec;

  const int val = 1;
  for (int opt = 0; sock_opts_mask; ++opt) {
    if (sock_opts_mask & 1) {
      if (setsockopt(fd_, SOL_SOCKET, opt, &val, sizeof(val)) < 0) {
        LOG(WARNING) << "setsockopt: could not set opt " << opt << ", " << strerror(errno);
      }
    }
    sock_opts_mask >>= 1;
  }

  sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (posix_err_wrap(bind(fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)), &ec) < 0)
    return ec;

  VSOCK(1) << "Listening";

  posix_err_wrap(listen(fd_, backlog), &ec);
  return ec;
}

void FiberSocket::SetController(EvController* p) {
  CHECK(p_ == nullptr);
  p_ = p;

  if (fd_ >= 0) {
    auto cb = [this](uint32 mask, EvController* cntr) { Wakey(mask, cntr); };
    arm_index_ = p->Arm(fd_ & FD_MASK, std::move(cb), EPOLLIN);
  }
}

auto FiberSocket::Accept(FiberSocket* peer) -> error_code {
  CHECK(p_);

  sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);

  error_code ec;

  int real_fd = fd_ & FD_MASK;

  current_context_ = fibers::context::active();

  while (true) {
    int res =
        accept4(real_fd, (struct sockaddr*)&client_addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (res >= 0) {
      *peer = FiberSocket{res};
      break;
    }

    DCHECK_EQ(-1, res);

    if (errno != EAGAIN) {
      ec = from_errno();
      break;
    }

    current_context_->suspend();
  }
  current_context_ = nullptr;
  return ec;
}

auto FiberSocket::Connect(const endpoint_type& ep) -> error_code {
  CHECK_EQ(fd_, -1);
  CHECK(p_ && p_->InMyThread());

  error_code ec;

  fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (posix_err_wrap(fd_, &ec) < 0)
    return ec;

  auto cb = [this](uint32 mask, EvController* cntr) { Wakey(mask, cntr); };
  arm_index_ = p_->Arm(fd_, std::move(cb), EPOLLIN);
  current_context_ = fibers::context::active();

  while (true) {
    int res = connect(fd_, ep.data(), ep.size());
    if (res == 0) {
      break;
    }

    if (errno != EINPROGRESS) {
      ec = from_errno();
      break;
    }
    current_context_->suspend();
  }
  current_context_ = nullptr;

  if (ec) {
    if (close(fd_) < 0) {
      LOG(WARNING) << "Could not close fd " << strerror(errno);
    }
    fd_ = -1;
  }
  return ec;
}

auto FiberSocket::LocalEndpoint() const -> endpoint_type {
  endpoint_type endpoint;

  if (fd_ < 0)
    return endpoint;
  socklen_t addr_len = endpoint.capacity();
  error_code ec;

  posix_err_wrap(::getsockname(fd_ & FD_MASK, endpoint.data(), &addr_len), &ec);
  CHECK(!ec) << ec << "/" << ec.message() << " while running getsockname";

  endpoint.resize(addr_len);

  return endpoint;
}

auto FiberSocket::RemoteEndpoint() const -> endpoint_type {
  endpoint_type endpoint;
  CHECK_GT(fd_, 0);

  socklen_t addr_len = endpoint.capacity();
  error_code ec;

  if (getpeername(fd_ & FD_MASK, endpoint.data(), &addr_len) == 0)
    endpoint.resize(addr_len);

  return endpoint;
}

auto FiberSocket::Send(const iovec* ptr, size_t len) -> expected_size_t {
  CHECK(p_);
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
  current_context_ = fibers::context::active();

  while (true) {
    res = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (res >= 0) {
      current_context_ = nullptr;
      return res;
    }

    DCHECK_EQ(res, -1);
    res = errno;

    if (res != EAGAIN) {
      break;
    }
    current_context_->suspend();
  }

  current_context_ = nullptr;

  // Error handling - finale part.
  if (!base::_in(res, {ECONNABORTED, EPIPE, ECONNRESET})) {
    LOG(FATAL) << "Unexpected error " << res << "/" << strerror(res);
  }

  if (res == EPIPE)  // We do not care about EPIPE that can happen when we shutdown our socket.
    res = ECONNABORTED;

  std::error_code ec(res, std::system_category());
  VSOCK(1) << "Error " << ec << " on " << RemoteEndpoint();

  return nonstd::make_unexpected(std::move(ec));
}

auto FiberSocket::Recv(iovec* ptr, size_t len) -> expected_size_t {
  CHECK_GT(len, 0U);
  CHECK(p_);
  CHECK_GE(fd_, 0);

  if (fd_ & IS_SHUTDOWN) {
    return nonstd::make_unexpected(std::make_error_code(std::errc::connection_aborted));
  }

  msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = const_cast<iovec*>(ptr);
  msg.msg_iovlen = len;

  int fd = fd_ & FD_MASK;
  current_context_ = fibers::context::active();

  ssize_t res;
  while (true) {
    res = recvmsg(fd, &msg, 0);
    if (res > 0) {  // if res is 0, that means a peer closed the socket.
      current_context_ = nullptr;
      return res;
    }

    if (res == 0 || errno != EAGAIN) {
      break;
    }

    current_context_->suspend();
  }

  current_context_ = nullptr;

  // Error handling - finale part.
  if (res == 0) {
    res = ECONNABORTED;
  } else {
    DCHECK_EQ(-1, res);
    res = errno;
  }

  DVSOCK(1) << "Got " << res;

  if (!base::_in(res, {ECONNABORTED, EPIPE, ECONNRESET})) {
    LOG(FATAL) << "sock[" << fd << "] Unexpected error " << res << "/" << strerror(res);
  }

  std::error_code ec(res, std::system_category());
  VSOCK(1) << "Error " << ec << " on " << RemoteEndpoint();

  return nonstd::make_unexpected(std::move(ec));
}

void FiberSocket::Wakey(uint32_t ev_mask, EvController* cntr) {
  DVLOG(2) << "Wakey " << ev_mask;
  if (current_context_)
    fibers::context::active()->schedule(current_context_);
}

}  // namespace epoll
}  // namespace util
