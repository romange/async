// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

// #include <boost/asio/ssl/error.hpp>

#include "util/tls/ssl_stream.h"

#include "base/logging.h"

namespace util {

namespace tls {

#if 0
using namespace boost;
using asio::ssl::detail::stream_core;

namespace detail {

using asio::ssl::stream_base;
using asio::ssl::detail::engine;

Engine::Engine(SSL_CTX* context) : ssl_(::SSL_new(context)) {
  CHECK(ssl_);

  ::SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
  ::SSL_set_mode(ssl_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  ::SSL_set_mode(ssl_, SSL_MODE_RELEASE_BUFFERS);

  ::BIO* int_bio = 0;
  ::BIO_new_bio_pair(&int_bio, 0, &ext_bio_, 0);
  ::SSL_set_bio(ssl_, int_bio, int_bio);
}

Engine::~Engine() {
  CHECK(!SSL_get_app_data(ssl_));

  ::BIO_free(ext_bio_);
  ::SSL_free(ssl_);
}

system::error_code Engine::set_verify_mode(verify_mode v, system::error_code& ec) {
  ::SSL_set_verify(ssl_, v, ::SSL_get_verify_callback(ssl_));

  ec = system::error_code();
  return ec;
}

engine::want Engine::perform(int (Engine::*op)(void*, std::size_t), void* data, std::size_t length,
                             system::error_code& ec, std::size_t* bytes_transferred) {
  std::size_t pending_output_before = ::BIO_ctrl_pending(ext_bio_);
  ::ERR_clear_error();
  int result = (this->*op)(data, length);
  int ssl_error = ::SSL_get_error(ssl_, result);
  int sys_error = static_cast<int>(::ERR_get_error());
  std::size_t pending_output_after = ::BIO_ctrl_pending(ext_bio_);

  if (ssl_error == SSL_ERROR_SSL) {
    ec = system::error_code(sys_error, asio::error::get_ssl_category());
    return pending_output_after > pending_output_before ? engine::want_output
                                                        : engine::want_nothing;
  }

  if (ssl_error == SSL_ERROR_SYSCALL) {
    if (sys_error == 0) {
      ec = asio::ssl::error::unspecified_system_error;
    } else {
      ec = system::error_code(sys_error, asio::error::get_ssl_category());
    }
    return pending_output_after > pending_output_before ? engine::want_output
                                                        : engine::want_nothing;
  }

  if (result > 0 && bytes_transferred)
    *bytes_transferred = static_cast<std::size_t>(result);

  if (ssl_error == SSL_ERROR_WANT_WRITE) {
    ec = system::error_code();
    return engine::want_output_and_retry;
  } else if (pending_output_after > pending_output_before) {
    ec = system::error_code();
    return result > 0 ? engine::want_output : engine::want_output_and_retry;
  } else if (ssl_error == SSL_ERROR_WANT_READ) {
    ec = system::error_code();
    return engine::want_input_and_retry;
  } else if (ssl_error == SSL_ERROR_ZERO_RETURN) {
    ec = asio::error::eof;
    return engine::want_nothing;
  } else if (ssl_error == SSL_ERROR_NONE) {
    ec = system::error_code();
    return engine::want_nothing;
  } else {
    ec = asio::ssl::error::unexpected_result;
    return engine::want_nothing;
  }
}

int Engine::do_connect(void*, std::size_t) {
  return ::SSL_connect(ssl_);
}

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

Engine::want Engine::handshake(stream_base::handshake_type type, system::error_code& ec) {
  CHECK_EQ(stream_base::client, type);

  return perform(&Engine::do_connect, 0, 0, ec, 0);
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

}  // namespace detail

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
