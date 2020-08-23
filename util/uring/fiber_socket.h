// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <liburing/io_uring.h>

// for tcp::endpoint. Consider introducing our own.
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "absl/base/attributes.h"
#include "util/sync_stream_interface.h"

namespace util {
namespace uring {

class Proactor;

class FiberSocket : public SyncStreamInterface {
  FiberSocket(const FiberSocket&) = delete;
  void operator=(const FiberSocket&) = delete;

  explicit FiberSocket(int fd) : fd_(fd), p_(nullptr) {
  }

 public:
  using native_handle_type = int;
  using endpoint_type = ::boost::asio::ip::tcp::endpoint;
  using error_code = std::error_code;
  using expected_size_t = nonstd::expected<size_t, error_code>;

  FiberSocket() : FiberSocket(-1) {
  }

  FiberSocket(FiberSocket&& other) noexcept : fd_(other.fd_), p_(other.p_) {
    other.fd_ = -1;
    other.p_ = nullptr;
  }

  virtual ~FiberSocket();

  FiberSocket& operator=(FiberSocket&& other) noexcept;

  // sock_opts are the bit mask of sockopt values shifted left, i.e.
  // (1 << SO_REUSEADDR) | (1 << SO_DONTROUTE), for example.
  ABSL_MUST_USE_RESULT error_code Listen(unsigned port, unsigned backlog,
                                         uint32_t sock_opts_mask = 0);

  ABSL_MUST_USE_RESULT error_code Accept(FiberSocket* peer);

  ABSL_MUST_USE_RESULT error_code Connect(const endpoint_type& ep);

  ABSL_MUST_USE_RESULT error_code Shutdown(int how);

  ABSL_MUST_USE_RESULT error_code Close();

  // Really need here expected.
  expected_size_t Send(const iovec* ptr, size_t len) override;

  expected_size_t Send(const boost::asio::const_buffer& b) {
    iovec v{const_cast<void*>(b.data()), b.size()};
    return Send(&v, 1);
  }

  expected_size_t Recv(iovec* ptr, size_t len) override;

  expected_size_t Recv(const boost::asio::mutable_buffer& mb) {
    iovec v{mb.data(), mb.size()};
    return Recv(&v, 1);
  }

  native_handle_type native_handle() const {
    return fd_ & FD_MASK;
  }

  //! Removes the ownership over file descriptor. Use with caution.
  void Detach() {
    fd_ = -1;
  }

  int RealFd() const;

  endpoint_type LocalEndpoint() const;
  endpoint_type RemoteEndpoint() const;

  //! IsOpen does not promise that the socket is TCP connected or live,
  // just that the file descriptor is valid and its state is open.
  bool IsOpen() const {
    return fd_ >= 0 && (fd_ & IS_SHUTDOWN) == 0;
  }

  void SetProactor(Proactor* p);

  Proactor* proactor() { return p_; }

  static bool IsConnClosed(const error_code& ec) {
    return (ec == std::errc::connection_aborted) || (ec == std::errc::connection_reset);
  }

 private:
  // Gives me 512M descriptors.
  enum { FD_MASK = 0x1fffffff };
  enum { IS_SHUTDOWN = 0x20000000, FIXED_FD = 0x40000000 };

  int32_t fd_;

  // We must reference proactor in each socket so that we could support write_some/read_some
  // with predefined interfance and be compliant with SyncWriteStream/SyncReadStream concepts.
  Proactor* p_;
};

}  // namespace uring
}  // namespace util
