// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <openssl/ossl_typ.h>

#include "util/fiber_socket_base.h"


namespace util {
namespace tls {

class Engine;

class TlsSocket : public FiberSocketBase {
 public:
  TlsSocket(FiberSocketBase* next = nullptr);

  ~TlsSocket();

  void InitSSL(SSL_CTX* context);

  error_code Shutdown(int how) final;

  accept_result Accept() final;

  error_code Connect(const endpoint_type& ep) final;

  error_code Close() final;

  expected_size_t RecvMsg(const msghdr& msg, int flags) final;

  expected_size_t Send(const iovec* ptr, size_t len) final;
  expected_size_t Recv(iovec* ptr, size_t len) final;

  SSL* ssl_handle();

 private:
  FiberSocketBase* next_sock_;
  std::unique_ptr<Engine> engine_;
};

}  // namespace tls
}  // namespace util
