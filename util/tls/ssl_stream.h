// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <absl/types/span.h>
#include <openssl/ssl.h>

#include <system_error>

#include "base/expected.hpp"

namespace util {
namespace tls {

namespace detail {

class error_category : public std::error_category {
 public:
  const char* name() const noexcept final {
    return "async.tls";
  }

  std::string message(int ev) const final;

  std::error_condition default_error_condition(int ev) const noexcept final;

  bool equivalent(int ev, const std::error_condition& condition) const noexcept final {
    return condition.value() == ev && &condition.category() == this;
  }

  bool equivalent(const std::error_code& error, int ev) const noexcept final {
    return error.value() == ev && &error.category() == this;
  }
};

class Engine {
 public:
  enum HandshakeType { CLIENT = 1, SERVER = 2 };

  using error_code = ::std::error_code;
  using Buffer = absl::Span<const uint8_t>;

  // If OpResult has error code then it's an openssl error (as returned by ERR_get_error()).
  // In that case the wrapping flow should be stopped since any error there is unretriable.
  // if OpResult has value, then its positive value means success depending on the context
  // of the operation. if value==-1 then it means that it should be retried.
  // In any case for non-error OpResult a caller must check OutputPending and write the output buffer
  // to the appropriate channel.
  using OpResult = nonstd::expected<int, unsigned long> ;
  using BufResult = nonstd::expected<Buffer, unsigned long> ;

  // Construct a new engine for the specified context.
  explicit Engine(SSL_CTX* context);

  // Destructor.
  ~Engine();

  // Get the underlying implementation in the native type.
  SSL* native_handle() {
    return ssl_;
  }

  //! Set the peer verification mode. mode is a mask of values specified at
  //! https://www.openssl.org/docs/man1.1.1/man3/SSL_set_verify.html
  void set_verify_mode(int mode) {
    ::SSL_set_verify(ssl_, mode, ::SSL_get_verify_callback(ssl_));
  }

  // Perform an SSL handshake using either SSL_connect (client-side) or
  // SSL_accept (server-side).
  OpResult Handshake(HandshakeType type);

  //! Returns output (read) buffer. This operation is destructive, i.e. after calling
  //! this function the buffer is being consumed.
  //! See OutputPending() for checking if there is a output buffer to consume.
  BufResult FetchOutputBuf();

  //! Returns output (read) buffer. This operation is not destructive and
  // following ConsumeOutputBuf should be called.
  BufResult PeekOutputBuf();

  // sz should be not greater than the buffer size from the last PeekOutputBuf() call.
  void ConsumeOutputBuf(unsigned sz);

  // Returns number of written bytes or the error.
  OpResult WriteBuf(const Buffer& buf);

#if 0
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

#endif

  size_t OutputPending() const {
    return BIO_ctrl(output_bio_, BIO_CTRL_PENDING, 0, NULL);
  }

  //! It's a bit confusing but when we write into output_bio_ it's like
  //! and input buffer to the engine.
  size_t InputPending() const {
    return BIO_ctrl(output_bio_, BIO_CTRL_WPENDING, 0, NULL);
  }

 private:
  // Disallow copying and assignment.
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;


  // Perform one operation. Returns > 0 on success.
  using EngineOp = int (Engine::*)(void*, std::size_t);

  OpResult Perform(EngineOp op, void* data, std::size_t length);

  // Adapt the SSL_connect function to the signature needed for perform().
  int do_connect(void*, std::size_t);

  int do_accept(void*, std::size_t);

#if 0
  // Adapt the SSL_shutdown function to the signature needed for perform().
  int do_shutdown(void*, std::size_t);

  // Adapt the SSL_read function to the signature needed for perform().
  int do_read(void* data, std::size_t length);

  // Adapt the SSL_write function to the signature needed for perform().
  int do_write(void* data, std::size_t length);
#endif

  SSL* ssl_;
  BIO* output_bio_;
};

}  // namespace detail

#if 0
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

#endif

}  // namespace tls
}  // namespace util
