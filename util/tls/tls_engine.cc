// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/tls/tls_engine.h"

#include <openssl/err.h>

#include "base/logging.h"

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#error Please update your libssl to libssl1.1 - install libssl-dev
#endif

namespace util {

namespace tls {

Engine::Engine(SSL_CTX* context) : ssl_(::SSL_new(context)) {
  CHECK(ssl_);

  SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_set_mode(ssl_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_set_mode(ssl_, SSL_MODE_RELEASE_BUFFERS);

  ::BIO* int_bio = 0;

  BIO_new_bio_pair(&int_bio, 0, &external_bio_, 0);

  // SSL_set0_[rw]bio take ownership of the passed reference,
  // so if we call both with the same BIO, we need the refcount to be 2.
  BIO_up_ref(int_bio);
  SSL_set0_rbio(ssl_, int_bio);
  SSL_set0_wbio(ssl_, int_bio);
}

Engine::~Engine() {
  CHECK(!SSL_get_app_data(ssl_));

  ::BIO_free(external_bio_);
  ::SSL_free(ssl_);
}

auto Engine::ToOpResult(const SSL* ssl, int result) -> Engine::OpResult {
  DCHECK_LE(result, 0);

  unsigned long error = ERR_get_error();
  if (error != 0) {
    return nonstd::make_unexpected(error);
  }

  int want = SSL_want(ssl);

  if (want == SSL_NOTHING) {
    int ssl_error = SSL_get_error(ssl, result);
    if (ssl_error == SSL_ERROR_ZERO_RETURN)
      return EOF_STREAM;
    LOG(FATAL) << "Unexpected error " << ssl_error;
  }
  if (SSL_WRITING == want)
    return Engine::NEED_WRITE;
  else if (SSL_READING == want)
    return Engine::NEED_READ_AND_MAYBE_WRITE;
  LOG(FATAL) << "Unsupported want value " << want;

  return EOF_STREAM;
}

#define RETURN_RESULT(res) \
  if (res > 0)             \
    return res;            \
  return ToOpResult(ssl_, res)

auto Engine::FetchOutputBuf() -> BufResult {
  char* buf = nullptr;

  int res = BIO_nread(external_bio_, &buf, INT_MAX);
  if (res < 0) {
    unsigned long error = ::ERR_get_error();
    return nonstd::make_unexpected(error);
  }

  return Buffer(reinterpret_cast<const uint8_t*>(buf), res);
}


// TODO: to consider replacing BufResult with Buffer since
// it seems BIO_C_NREAD0 should not return negative values when used properly.
auto Engine::PeekOutputBuf() -> BufResult {
  char* buf = nullptr;

  long res = BIO_ctrl(external_bio_, BIO_C_NREAD0, 0, &buf);
  if (res == -1) {  // no data
    res = 0;
  } else if (res > INT_MAX) {
    res = INT_MAX;
  }
  CHECK_GE(res, 0);
  return Buffer(reinterpret_cast<const uint8_t*>(buf), res);
}

void Engine::ConsumeOutputBuf(unsigned sz) {
  int res = BIO_nread(external_bio_, NULL, sz);
  CHECK_GT(res, 0);
  CHECK_EQ(unsigned(res), sz);
}

auto Engine::WriteBuf(const Buffer& buf) -> OpResult {
  DCHECK(!buf.empty());

  char* cbuf = nullptr;
  int res = BIO_nwrite(external_bio_, &cbuf, buf.size());
  if (res < 0) {
    unsigned long error = ::ERR_get_error();
    return nonstd::make_unexpected(error);
  } else if (res > 0) {
    memcpy(cbuf, buf.data(), res);
  }
  return res;
}

auto Engine::PeekInputBuf() const -> MutableBuffer {
  char* buf = nullptr;

  int res = BIO_nwrite0(external_bio_, &buf);
  CHECK_GT(res, 0);

  return MutableBuffer{reinterpret_cast<uint8_t*>(buf), unsigned(res)};
}

void Engine::CommitInput(unsigned sz) {
  CHECK_LE(sz, unsigned(INT_MAX));

  CHECK_EQ(int(sz), BIO_nwrite(external_bio_, nullptr, sz));
}

auto Engine::Handshake(HandshakeType type) -> OpResult {
  int result = (type == CLIENT) ? SSL_connect(ssl_) : SSL_accept(ssl_);
  RETURN_RESULT(result);
}

auto Engine::Shutdown() -> OpResult {
  int result = SSL_shutdown(ssl_);
  // See https://www.openssl.org/docs/man1.1.1/man3/SSL_shutdown.html
  if (result == 0)  // First step of Shutdown (close_notify) returns 0.
    return result;

  RETURN_RESULT(result);
}

auto Engine::Write(const Buffer& buf) -> OpResult {
  if (buf.empty())
    return 0;
  int sz = buf.size() < INT_MAX ? buf.size() : INT_MAX;
  int result = SSL_write(ssl_, buf.data(), sz);
  RETURN_RESULT(result);
}

auto Engine::Read(uint8_t* dest, size_t len) -> OpResult {
  if (len == 0)
    return 0;
  int sz = len < INT_MAX ? len : INT_MAX;
  int result = SSL_read(ssl_, dest, sz);

  RETURN_RESULT(result);
}


#if 0
SslStream::SslStream(FiberSyncSocket&& arg, asio::ssl::context& ctx)
    : engine_(ctx.native_handle()), next_layer_(std::move(arg)) {
}

void SslStream::handshake(Impl::handshake_type type, error_code& ec) {
  namespace a = ::boost::asio;
  auto cb = [&](detail::Engine& eng, error_code& ec, size_t& bytes_transferred) {
    bytes_transferred = 0;
    return eng.handshake(type, ec);
  };

  IoLoop(cb, ec);
}

void SslStream::IoHandler(want op_code, system::error_code& ec) {
  using asio::ssl::detail::engine;
  DVLOG(1) << "io_fun::start";
  asio::mutable_buffer mb;
  asio::const_buffer cbuf;
  size_t buf_size;

  switch (op_code) {
    case engine::want_input_and_retry:
      DVLOG(2) << "want_input_and_retry";
      engine_.GetWriteBuf(&mb);
      buf_size = next_layer_.read_some(mb, ec);
      if (!ec) {
        engine_.CommitWriteBuf(buf_size);
      }
      break;

    case engine::want_output_and_retry:
    case engine::want_output:

      DVLOG(2) << "engine::want_output"
               << (op_code == engine::want_output_and_retry ? "_and_retry" : "");
      engine_.GetReadBuf(&cbuf);

      // Get output data from the engine and write it to the underlying
      // transport.

      asio::write(next_layer_, cbuf, ec);
      if (!ec) {
        engine_.AdvanceRead(cbuf.size());
      }
      break;

    default:;
  }
}

#endif

}  // namespace tls
}  // namespace util
