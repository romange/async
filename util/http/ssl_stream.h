// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <boost/asio/ssl/stream.hpp>
#include "util/asio/fiber_socket.h"

namespace util {
namespace http {

namespace detail {

class Engine {
 public:
  using verify_mode = ::boost::asio::ssl::verify_mode;
  using want = ::boost::asio::ssl::detail::engine::want;

  // Construct a new engine for the specified context.
  explicit Engine(SSL_CTX* context);

  // Destructor.
  ~Engine();

  // Get the underlying implementation in the native type.
  SSL* native_handle() {
    return ssl_;
  }

  // Set the peer verification mode.
  boost::system::error_code set_verify_mode(verify_mode v, boost::system::error_code& ec);

  // Perform an SSL handshake using either SSL_connect (client-side) or
  // SSL_accept (server-side).
  want handshake(::boost::asio::ssl::stream_base::handshake_type type,
                 boost::system::error_code& ec);

  // Perform a graceful shutdown of the SSL session.
  want shutdown(boost::system::error_code& ec);

  // Write bytes to the SSL session.
  want write(const boost::asio::const_buffer& data, boost::system::error_code& ec,
             std::size_t& bytes_transferred);

  // Read bytes from the SSL session.
  want read(const boost::asio::mutable_buffer& data, boost::system::error_code& ec,
            std::size_t& bytes_transferred);


  void GetWriteBuf(boost::asio::mutable_buffer* mbuf);

  //! sz should be less or equal to the size returned by GetWriteBuf.
  void CommitWriteBuf(size_t sz);

  void GetReadBuf(boost::asio::const_buffer* cbuf);
  void AdvanceRead(size_t sz);

  // Map an error::eof code returned by the underlying transport according to
  // the type and state of the SSL session. Returns a const reference to the
  // error code object, suitable for passing to a completion handler.
  const boost::system::error_code& map_error_code(boost::system::error_code& ec) const;

 private:
  // Disallow copying and assignment.
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Perform one operation. Returns >= 0 on success or error, want_read if the
  // operation needs more input, or want_write if it needs to write some output
  // before the operation can complete.
  want perform(int (Engine::*op)(void*, std::size_t), void* data, std::size_t length,
               boost::system::error_code& ec, std::size_t* bytes_transferred);

  // Adapt the SSL_connect function to the signature needed for perform().
  int do_connect(void*, std::size_t);

  // Adapt the SSL_shutdown function to the signature needed for perform().
  int do_shutdown(void*, std::size_t);

  // Adapt the SSL_read function to the signature needed for perform().
  int do_read(void* data, std::size_t length);

  // Adapt the SSL_write function to the signature needed for perform().
  int do_write(void* data, std::size_t length);

  SSL* ssl_;
  BIO* ext_bio_;
};

}  // namespace detail

class SslStream {
  using Impl = ::boost::asio::ssl::stream<FiberSyncSocket>;

  SslStream(const SslStream&) = delete;
  SslStream& operator=(const SslStream&) = delete;

 public:
  using next_layer_type = Impl::next_layer_type;
  using lowest_layer_type = Impl::lowest_layer_type;
  using error_code = boost::system::error_code;

  SslStream(FiberSyncSocket&& arg, ::boost::asio::ssl::context& ctx);

  // To support socket requirements.
  next_layer_type& next_layer() {
    return next_layer_;
  }

  lowest_layer_type& lowest_layer() {
    return next_layer_.lowest_layer();
  }

  // (fiber) SyncRead interface:
  // https://www.boost.org/doc/libs/1_69_0/doc/html/boost_asio/reference/SyncReadStream.html
  template <typename MBS> size_t read_some(const MBS& bufs, error_code& ec) {
    namespace a = ::boost::asio;

    auto cb = [&](detail::Engine& eng, error_code& ec, size_t& bytes_transferred) {
      a::mutable_buffer buffer =
          a::detail::buffer_sequence_adapter<a::mutable_buffer, MBS>::first(bufs);

      return eng.read(buffer, ec, bytes_transferred);
    };

    size_t res = IoLoop(cb, ec);
    last_err_ = ec;
    return res;
  }

  //! To calm SyncReadStream compile-checker we provide exception-enabled interface without
  //! implementing it.
  template <typename MBS> size_t read_some(const MBS& bufs);

  //! SyncWrite interface:
  //! https://www.boost.org/doc/libs/1_69_0/doc/html/boost_asio/reference/SyncWriteStream.html
  template <typename BS> size_t write_some(const BS& bufs, error_code& ec) {
    namespace a = ::boost::asio;
    auto cb = [&](detail::Engine& eng, error_code& ec, size_t& bytes_transferred) {
      a::const_buffer buffer = a::detail::buffer_sequence_adapter<a::const_buffer, BS>::first(bufs);

      return eng.write(buffer, ec, bytes_transferred);
    };

    size_t res = IoLoop(cb, ec);
    last_err_ = ec;
    return res;
  }

  //! To calm SyncWriteStream compile-checker we provide exception-enabled interface without
  //! implementing it.
  template <typename BS> size_t write_some(const BS& bufs);

  void handshake(Impl::handshake_type type, error_code& ec);

  const error_code& last_error() const {
    return last_err_;
  }

  auto native_handle() {
    return engine_.native_handle();
  }

 private:
  using want = detail::Engine::want;

  template <typename Operation>
  std::size_t IoLoop(const Operation& op, boost::system::error_code& ec);

  void IoHandler(want op_code, boost::system::error_code& ec);

  detail::Engine engine_;
  FiberSyncSocket next_layer_;

  error_code last_err_;
};

template <typename Operation>
std::size_t SslStream::IoLoop(const Operation& op, boost::system::error_code& ec) {
  using engine = ::boost::asio::ssl::detail::engine;

  std::size_t bytes_transferred = 0;
  want op_code = engine::want_nothing;

  do {
    op_code = op(engine_, ec, bytes_transferred);
    if (ec)
      break;
    IoHandler(op_code, ec);
  } while (!ec && int(op_code) < 0);

  if (ec) {
    // Operation failed. Return result to caller.
    engine_.map_error_code(ec);
    return 0;
  } else {
    return bytes_transferred;
  }
}

}  // namespace http
}  // namespace util
