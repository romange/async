// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/tls/ssl_stream.h"

#include <openssl/err.h>

#include "base/logging.h"

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#error Please update your libssl to libssl1.1 - install libssl-dev
#endif


namespace util {

namespace tls {

namespace detail {

static error_category tls_category;

std::string error_category::message(int ev) const {
  const char* s = ::ERR_reason_error_string(ev);
  return s ? s : "undefined";
}

std::error_condition error_category::default_error_condition(int ev) const noexcept {
  return std::error_condition{ev, *this};
}


Engine::Engine(SSL_CTX* context) : ssl_(::SSL_new(context)) {
  CHECK(ssl_);

  SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_set_mode(ssl_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_set_mode(ssl_, SSL_MODE_RELEASE_BUFFERS);

  ::BIO* int_bio = 0;

  // Deliberetely small buffers.
  BIO_new_bio_pair(&int_bio, 32, &output_bio_, 32);

  // SSL_set0_[rw]bio take ownership of the passed reference,
  // so if we call both with the same BIO, we need the refcount to be 2.
  BIO_up_ref(int_bio);
  SSL_set0_rbio(ssl_, int_bio);
  SSL_set0_wbio(ssl_, int_bio);
}

Engine::~Engine() {
  CHECK(!SSL_get_app_data(ssl_));

  ::BIO_free(output_bio_);
  ::SSL_free(ssl_);
}

Engine::OpResult Engine::Perform(EngineOp op, void* data, std::size_t length) {
  int result = (this->*op)(data, length);

  if (result > 0) {
    return result;
  }

  unsigned long error = ERR_get_error();
  if (error != 0) {
    return nonstd::make_unexpected(error);
  }

  int want = SSL_want(ssl_);
  CHECK(want == SSL_WRITING || want == SSL_READING) << want;

  return -1;
}

auto Engine::GetOutputBuf() -> BufResult {
  char* buf = nullptr;

  int res = BIO_nread(output_bio_, &buf, INT_MAX);
  if (res < 0) {
    unsigned long error = ::ERR_get_error();
    return nonstd::make_unexpected(error);
  }

  return Buffer(reinterpret_cast<const uint8_t*>(buf), res);
}

auto Engine::WriteBuf(const Buffer& buf) -> OpResult {
  DCHECK(!buf.empty());

  char* cbuf = nullptr;
  int res = BIO_nwrite(output_bio_, &cbuf, buf.size());
  if (res < 0) {
    unsigned long error = ::ERR_get_error();
    return nonstd::make_unexpected(error);
  } else if (res > 0) {
    memcpy(cbuf, buf.data(), res);
  }
  return res;
}

int Engine::do_connect(void*, std::size_t) {
  return SSL_connect(ssl_);
}

int Engine::do_accept(void*, std::size_t) {
  return ::SSL_accept(ssl_);
}

auto Engine::Handshake(HandshakeType type) -> OpResult {
  if (type == CLIENT)
    return Perform(&Engine::do_connect, 0, 0);
  else
    return Perform(&Engine::do_accept, 0, 0);
}


#if 0

int Engine::do_shutdown(void*, std::size_t) {
  int result = ::SSL_shutdown(ssl_);
  if (result == 0)
    result = ::SSL_shutdown(ssl_);
  return result;
}

int Engine::do_read(void* data, std::size_t length) {
  return ::SSL_read(ssl_, data, length < INT_MAX ? static_cast<int>(length) : INT_MAX);
}

int Engine::do_write(void* data, std::size_t length) {
  return ::SSL_write(ssl_, data, length < INT_MAX ? static_cast<int>(length) : INT_MAX);
}


Engine::want Engine::shutdown(system::error_code& ec) {
  return perform(&Engine::do_shutdown, 0, 0, ec, 0);
}

Engine::want Engine::write(const asio::const_buffer& data, system::error_code& ec,
                           std::size_t& bytes_transferred) {
  if (data.size() == 0) {
    ec = system::error_code();
    return engine::want_nothing;
  }

  return perform(&Engine::do_write, const_cast<void*>(data.data()), data.size(), ec,
                 &bytes_transferred);
}

Engine::want Engine::read(const asio::mutable_buffer& data, system::error_code& ec,
                          std::size_t& bytes_transferred) {
  if (data.size() == 0) {
    ec = system::error_code();
    return engine::want_nothing;
  }

  return perform(&Engine::do_read, data.data(), data.size(), ec, &bytes_transferred);
}

void Engine::GetWriteBuf(asio::mutable_buffer* mbuf) {
  char* buf = nullptr;

  int res = BIO_nwrite0(ext_bio_, &buf);
  CHECK_GE(res, 0);
  *mbuf = asio::mutable_buffer{buf, size_t(res)};
}

void Engine::CommitWriteBuf(size_t sz) {
  CHECK_EQ(sz, BIO_nwrite(ext_bio_, nullptr, sz));
}

void Engine::GetReadBuf(asio::const_buffer* cbuf) {
  char* buf = nullptr;

  int res = BIO_nread0(ext_bio_, &buf);
  CHECK_GE(res, 0);
  *cbuf = asio::const_buffer{buf, size_t(res)};
}

void Engine::AdvanceRead(size_t sz) {
  CHECK_EQ(sz, BIO_nread(ext_bio_, nullptr, sz));
}

const system::error_code& Engine::map_error_code(system::error_code& ec) const {
  // We only want to map the error::eof code.
  if (ec != asio::error::eof)
    return ec;

  // If there's data yet to be read, it's an error.
  if (BIO_wpending(ext_bio_)) {
    ec = asio::ssl::error::stream_truncated;
    return ec;
  }

  // SSL v2 doesn't provide a protocol-level shutdown, so an eof on the
  // underlying transport is passed through.
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
  if (SSL_version(ssl_) == SSL2_VERSION)
    return ec;
#endif  // (OPENSSL_VERSION_NUMBER < 0x10100000L)

  // Otherwise, the peer should have negotiated a proper shutdown.
  if ((::SSL_get_shutdown(ssl_) & SSL_RECEIVED_SHUTDOWN) == 0) {
    ec = asio::ssl::error::stream_truncated;
  }

  return ec;
}

#endif

}  // namespace detail

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
