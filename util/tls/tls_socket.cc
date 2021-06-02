// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/tls/tls_socket.h"

#include <openssl/err.h>

#include "base/logging.h"
#include "util/tls/tls_engine.h"

namespace util {
namespace tls {

using namespace boost;
using namespace std;
using nonstd::make_unexpected;

namespace {

class error_category : public std::error_category {
 public:
  const char* name() const noexcept final {
    return "async.tls";
  }

  string message(int ev) const final;

  error_condition default_error_condition(int ev) const noexcept final;

  bool equivalent(int ev, const error_condition& condition) const noexcept final {
    return condition.value() == ev && &condition.category() == this;
  }

  bool equivalent(const error_code& error, int ev) const noexcept final {
    return error.value() == ev && &error.category() == this;
  }
};

string error_category::message(int ev) const {
  char buf[256];
  ERR_error_string_n(ev, buf, sizeof(buf));
  return buf;
}

error_condition error_category::default_error_condition(int ev) const noexcept {
  return error_condition{ev, *this};
}

error_category tls_category;

inline error_code SSL2Error(unsigned long err) {
  CHECK_LT(err, unsigned(INT_MAX));

  return error_code{int(err), tls_category};
}

}  // namespace

static TlsSocket socket;

TlsSocket::TlsSocket(FiberSocketBase* next)
    : FiberSocketBase(next ? next->proactor() : nullptr), next_sock_(next) {
}

TlsSocket::~TlsSocket() {
}

void TlsSocket::InitSSL(SSL_CTX* context) {
  CHECK(!engine_);
  engine_.reset(new Engine{context});
}

auto TlsSocket::Shutdown(int how) -> error_code {
  return error_code{};
}

auto TlsSocket::Accept() -> accept_result {
  unsigned input_pending = 1;
  while (true) {
    Engine::OpResult op_result = engine_->Handshake(Engine::SERVER);
    if (!op_result) {
      return make_unexpected(SSL2Error(op_result.error()));
    }

    unsigned output_pending = engine_->OutputPending();
    if (output_pending > 0) {
      auto buf_result = engine_->PeekOutputBuf();
      CHECK(buf_result);
      CHECK(!buf_result->empty());

      auto snd_buf = asio::buffer(buf_result->data(), buf_result->size());

      expected_size_t write_result = next_sock_->Send(snd_buf);
      if (!write_result) {
        return make_unexpected(write_result.error());
      }
      CHECK_GT(*write_result, 0u);
      engine_->ConsumeOutputBuf(*write_result);
    }

    if (*op_result >= 0) {  // Shutdown or empty read/write may return 0.
      break;
    }
    if (*op_result == Engine::EOF_STREAM) {
      return make_unexpected(make_error_code(errc::connection_reset));
    }
    CHECK_EQ(Engine::RETRY, *op_result);

    input_pending = engine_->InputPending();
    VLOG(1) << "Input size: " << input_pending;
  }

  return nullptr;
}

auto TlsSocket::Connect(const endpoint_type& ep) -> error_code {
  return error_code{};
}

auto TlsSocket::Close() -> error_code {
  return error_code{};
}

auto TlsSocket::RecvMsg(const msghdr& msg, int flags) -> expected_size_t {
  return expected_size_t{};
}

auto TlsSocket::Send(const iovec* ptr, size_t len) -> expected_size_t {
  return expected_size_t{};
}

auto TlsSocket::Recv(iovec* ptr, size_t len) -> expected_size_t {
  return expected_size_t{};
}

SSL* TlsSocket::ssl_handle() {
  return engine_ ? engine_->native_handle() : nullptr;
}

}  // namespace tls
}  // namespace util
