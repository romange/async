// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/fiber_socket_base.h"

#include <netinet/in.h>
#include <sys/poll.h>

#include <boost/fiber/context.hpp>

#include "base/logging.h"
#include "base/stl_util.h"

#define VSOCK(verbosity) VLOG(verbosity) << "sock[" << native_handle() << "] "
#define DVSOCK(verbosity) DVLOG(verbosity) << "sock[" << native_handle() << "] "

namespace util {

using namespace std;
using namespace boost;

namespace {

inline ssize_t posix_err_wrap(ssize_t res, FiberSocketBase::error_code* ec) {
  if (res == -1) {
    *ec = FiberSocketBase::error_code(errno, std::system_category());
  } else if (res < 0) {
    LOG(WARNING) << "Bad posix error " << res;
  }
  return res;
}

}  // namespace

FiberSocketBase::~FiberSocketBase() {
  int fd = native_handle();

  if (fd > -1) {
    LOG(WARNING) << "Socket was not closed properly, closing file descriptor";
    int res = close(fd);
    LOG_IF(WARNING, res == -1) << "Error closing socket " << strerror(errno);
  }
}

auto FiberSocketBase::Shutdown(int how) -> error_code {
  CHECK_GE(fd_, 0);

  // If we shutdown and then try to Send/Recv - the call will stall since no data
  // is sent/received. Therefore we remember the state to allow consistent API experience.
  error_code ec;
  if (fd_ & IS_SHUTDOWN)
    return ec;
  int fd = native_handle();

  posix_err_wrap(::shutdown(fd, how), &ec);
  fd_ |= IS_SHUTDOWN;  // Enter shutdown state unrelated to the success of the call.

  return ec;
}


auto FiberSocketBase::Listen(unsigned port, unsigned backlog, uint32_t sock_opts_mask) -> error_code {
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

  OnSetProactor();
  return ec;
}

auto FiberSocketBase::LocalEndpoint() const -> endpoint_type {
  endpoint_type endpoint;

  if (fd_ < 0)
    return endpoint;
  socklen_t addr_len = endpoint.capacity();
  error_code ec;

  posix_err_wrap(::getsockname(native_handle(), endpoint.data(), &addr_len), &ec);
  CHECK(!ec) << ec << "/" << ec.message() << " while running getsockname";

  endpoint.resize(addr_len);

  return endpoint;
}

auto FiberSocketBase::RemoteEndpoint() const -> endpoint_type {
  endpoint_type endpoint;
  CHECK_GT(fd_, 0);

  socklen_t addr_len = endpoint.capacity();
  error_code ec;

  if (getpeername(native_handle(), endpoint.data(), &addr_len) == 0)
    endpoint.resize(addr_len);

  return endpoint;
}

void FiberSocketBase::SetProactor(ProactorBase* p) {
  CHECK(proactor_ == nullptr);
  proactor_ = p;

  OnSetProactor();
}

auto FiberSocketBase::Recv(iovec* ptr, size_t len) -> expected_size_t {
  CHECK_GT(len, 0U);

  msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = const_cast<iovec*>(ptr);
  msg.msg_iovlen = len;

  return RecvMsg(msg, 0);
}

}  // namespace util
