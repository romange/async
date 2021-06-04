// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

// for tcp::endpoint. Consider introducing our own.
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "absl/base/attributes.h"
#include "util/sync_stream_interface.h"

namespace util {

class ProactorBase;

class FiberSocketBase : public SyncStreamInterface {
  FiberSocketBase(const FiberSocketBase&) = delete;
  void operator=(const FiberSocketBase&) = delete;
  FiberSocketBase(FiberSocketBase&& other) = delete;
  FiberSocketBase& operator=(FiberSocketBase&& other) = delete;

 protected:
  explicit FiberSocketBase(ProactorBase* pb) : proactor_(pb) {
  }

 public:
  using endpoint_type = ::boost::asio::ip::tcp::endpoint;
  using error_code = std::error_code;
  using expected_size_t = nonstd::expected<size_t, error_code>;
  using accept_result = nonstd::expected<FiberSocketBase*, error_code>;

  ABSL_MUST_USE_RESULT virtual error_code Shutdown(int how) = 0;

  ABSL_MUST_USE_RESULT virtual accept_result Accept() = 0;

  ABSL_MUST_USE_RESULT virtual error_code Connect(const endpoint_type& ep) = 0;

  ABSL_MUST_USE_RESULT virtual error_code Close() = 0;

  virtual bool IsOpen() const = 0;

  expected_size_t virtual RecvMsg(const msghdr& msg, int flags) = 0;

  using SyncStreamInterface::Send;

  expected_size_t Send(const boost::asio::const_buffer& b) {
    iovec v{const_cast<void*>(b.data()), b.size()};
    return Send(&v, 1);
  }

  expected_size_t Recv(iovec* ptr, size_t len) override;

  expected_size_t Recv(const boost::asio::mutable_buffer& mb) {
    iovec v{mb.data(), mb.size()};
    return Recv(&v, 1);
  }

  static bool IsConnClosed(const error_code& ec) {
    return (ec == std::errc::connection_aborted) || (ec == std::errc::connection_reset);
  }

  void SetProactor(ProactorBase* p);

  ProactorBase* proactor() {
    return proactor_;
  }

 protected:
  virtual void OnSetProactor(){};

 private:
  // We must reference proactor in each socket so that we could support write_some/read_some
  // with predefined interfance and be compliant with SyncWriteStream/SyncReadStream concepts.
  ProactorBase* proactor_;
};

class LinuxSocketBase : public FiberSocketBase {
 public:
  using native_handle_type = int;

  virtual ~LinuxSocketBase();

  native_handle_type native_handle() const {
    return fd_ < 0 ? fd_ : fd_ & FD_MASK;
  }

  // sock_opts are the bit mask of sockopt values shifted left, i.e.
  // (1 << SO_REUSEADDR) | (1 << SO_DONTROUTE), for example.
  ABSL_MUST_USE_RESULT virtual error_code Listen(unsigned port, unsigned backlog,
                                                 uint32_t sock_opts_mask = 0);

  error_code Shutdown(int how) override;

  //! Removes the ownership over file descriptor. Use with caution.
  void Detach() {
    fd_ = -1;
  }

  //! IsOpen does not promise that the socket is TCP connected or live,
  // just that the file descriptor is valid and its state is open.
  bool IsOpen() const final {
    return fd_ >= 0 && (fd_ & IS_SHUTDOWN) == 0;
  }


  endpoint_type LocalEndpoint() const;
  endpoint_type RemoteEndpoint() const;


 protected:
  LinuxSocketBase(int fd, ProactorBase* pb) : FiberSocketBase(pb), fd_(fd) {}

  // Gives me 512M descriptors.
  enum { FD_MASK = 0x1fffffff };
  enum { IS_SHUTDOWN = 0x20000000 };

  int32_t fd_;
};

}  // namespace util
