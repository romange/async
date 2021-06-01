// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/tls/ssl_stream.h"

#include <absl/strings/string_view.h>
#include <openssl/err.h>

#include <boost/fiber/fiber.hpp>
#include <boost/fiber/operations.hpp>

#include "base/gtest.h"
#include "base/logging.h"

namespace util {
namespace tls {

using namespace std;
using namespace boost;
using detail::Engine;

class SslStreamTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
  }

  void SetUp() override;

  void TearDown() {
    client_engine_.reset();
    server_engine_.reset();
  }

  using OpCb = std::function<Engine::OpResult(Engine*)>;
  struct Options {
    string name;

    unsigned mutate_indx = 0;
    uint8_t mutate_val = 0;

    Options(absl::string_view v) : name{v} {
    }
  };

  static unsigned long RunPeer(Options opts, OpCb cb, Engine* src, Engine* dest);

  static string SSLError(unsigned long);

  SSL_CTX* CreateSslCntx();

  unique_ptr<detail::Engine> client_engine_, server_engine_;
  OpCb srv_handshake_, client_handshake_, read_op_, shutdown_op_;

  Options client_opts_{"client"}, srv_opts_{"server"};

  unique_ptr<uint8_t[]> read_buf_;
  enum { READ_CAPACITY = 1024 };
  size_t read_sz_ = 0;
};

int verify_callback(int ok, X509_STORE_CTX* ctx) {
  LOG(WARNING) << "verify_callback: " << ok;
  return 1;
}

string SslStreamTest::SSLError(unsigned long e) {
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  return buf;
}

SSL_CTX* SslStreamTest::CreateSslCntx() {
  SSL_CTX* ctx = SSL_CTX_new(TLS_method());

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  SSL_CTX_set_options(ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);

  // SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, verify_callback);

  // Can also be defined using "ADH:@SECLEVEL=0" cipher string below.
  SSL_CTX_set_security_level(ctx, 0);

  CHECK_EQ(1, SSL_CTX_set_cipher_list(ctx, "ADH"));

  return ctx;
}

void SslStreamTest::SetUp() {
  SSL_CTX* ctx = CreateSslCntx();
  client_engine_.reset(new detail::Engine(ctx));
  server_engine_.reset(new detail::Engine(ctx));
  SSL_CTX_free(ctx);

  // Configure server side.
  SSL* ssl = server_engine_->native_handle();
  SSL_set_options(ssl, SSL_OP_CIPHER_SERVER_PREFERENCE);
  CHECK_EQ(1, SSL_set_dh_auto(ssl, 1));

  read_buf_.reset(new uint8_t[READ_CAPACITY]);

  srv_handshake_ = [](Engine* eng) { return eng->Handshake(Engine::SERVER); };
  client_handshake_ = [](Engine* eng) { return eng->Handshake(Engine::CLIENT); };
  read_op_ = [this](Engine* eng) { return eng->Read(read_buf_.get(), READ_CAPACITY); };
  shutdown_op_ = [](Engine* eng) { return eng->Shutdown(); };
}

unsigned long SslStreamTest::RunPeer(Options opts, OpCb cb, Engine* src, Engine* dest) {
  ERR_print_errors_fp(stderr);  // Empties the queue.
  unsigned input_pending = 0;
  while (true) {
    auto op_result = cb(src);
    if (!op_result) {
      return op_result.error();
    }
    VLOG(1) << opts.name << " OpResult: " << *op_result;
    unsigned output_pending = src->OutputPending();
    if (output_pending > 0) {
      auto buf_result = src->PeekOutputBuf();
      CHECK(buf_result);
      VLOG(1) << opts.name << " wrote " << buf_result->size() << " bytes";
      CHECK(!buf_result->empty());

      if (opts.mutate_indx) {
        uint8_t* mem = const_cast<uint8_t*>(buf_result->data());
        mem[opts.mutate_indx % buf_result->size()] = opts.mutate_val;
        opts.mutate_indx = 0;
      }

      auto write_result = dest->WriteBuf(*buf_result);
      if (!write_result) {
        return write_result.error();
      }
      CHECK_GT(*write_result, 0);
      src->ConsumeOutputBuf(*write_result);
    }

    if (*op_result >= 0) {  // Shutdown or empty read/write may return 0.
      return 0;
    }
    if (*op_result == Engine::EOF_STREAM) {
      LOG(ERROR) << opts.name << " stream truncated";
      return 0;
    }
    CHECK_EQ(Engine::RETRY, *op_result);

    if (input_pending == 0 && output_pending == 0) {  // dropped connection
      LOG(INFO) << "Dropped connections for " << opts.name;

      return ERR_PACK(ERR_LIB_USER, 0, ERR_R_OPERATION_FAIL);
    }
    this_fiber::yield();
    input_pending = src->InputPending();
    VLOG(1) << "Input size: " << input_pending;
  }
}

TEST_F(SslStreamTest, BIO_s_bio_err) {
  // BIO_new creates a default buffer of 17KB.
  BIO* bio1 = BIO_new(BIO_s_bio());
  constexpr char kData[] = "ROMAN";
  ASSERT_EQ(0, ERR_get_error());

  // -2 - not implemented: https://www.openssl.org/docs/man1.1.1/man3/BIO_write.html
  EXPECT_EQ(-2, BIO_write(bio1, kData, sizeof(kData)));
  int e = ERR_get_error();
  EXPECT_NE(0, e);
  EXPECT_EQ(BIO_F_BIO_WRITE_INTERN, ERR_GET_FUNC(e));
  EXPECT_EQ(BIO_R_UNINITIALIZED, ERR_GET_REASON(e));
  BIO_free(bio1);
}

TEST_F(SslStreamTest, BIO_s_bio_ZeroCopy) {
  BIO *bio1, *bio2;

  // writesize = 0, means we use default 17KB buffer.
  ASSERT_EQ(1, BIO_new_bio_pair(&bio1, 0, &bio2, 0));

  char *buf1 = nullptr, *buf2 = nullptr;

  // Fetch buffer to the direct memory buffer.
  ssize_t write_size = BIO_nwrite0(bio1, &buf1);
  EXPECT_EQ(17 * 1024, write_size);
  memset(buf1, 'a', write_size);

  EXPECT_EQ(write_size, BIO_nwrite(bio1, nullptr, write_size));  // commit.
  EXPECT_EQ(-1, BIO_nwrite(bio1, nullptr, 1));                   // No space to commit.
  EXPECT_EQ(0, BIO_ctrl_get_write_guarantee(bio1));
  EXPECT_EQ(0, BIO_ctrl_pending(bio1));            // 0 read pending.
  EXPECT_EQ(write_size, BIO_ctrl_wpending(bio1));  // 17KB write pending.

  EXPECT_EQ(write_size, BIO_ctrl_pending(bio2));  // 17KB read pending.
  EXPECT_EQ(0, BIO_ctrl_wpending(bio2));          // 0 write pending.

  ASSERT_EQ(write_size, BIO_nread0(bio2, &buf2));
  EXPECT_EQ(0, memcmp(buf1, buf2, write_size));
  EXPECT_EQ(write_size, BIO_nread(bio2, nullptr, write_size));

  // bio1 is empty again.
  EXPECT_EQ(write_size, BIO_ctrl_get_write_guarantee(bio1));

  // Another way to write to BIO without using BIO_nwrite0.
  // Instead of fetching a pointer to buffer, we commit first and then write using memcpy.
  buf1 = nullptr;
  EXPECT_EQ(write_size, BIO_nwrite(bio1, &buf1, write_size));  // commit first.

  // BIO is single-threaded object, hence noone will read from bio2 as long
  // as we do not switch to reading ourselves. Therefore the order of committing and writing is not
  // important.
  memset(buf1, 'a', write_size);

  BIO_free(bio1);
  BIO_free(bio2);
}

TEST_F(SslStreamTest, Handshake) {
  unsigned long cl_err = 0, srv_err = 0;

  auto client_fb = fibers::fiber([&] {
    cl_err = RunPeer(client_opts_, client_handshake_, client_engine_.get(), server_engine_.get());
  });

  auto server_fb = fibers::fiber([&] {
    srv_err = RunPeer(srv_opts_, srv_handshake_, server_engine_.get(), client_engine_.get());
  });

  client_fb.join();
  server_fb.join();
  ASSERT_EQ(0, cl_err);
  ASSERT_EQ(0, srv_err);
}

TEST_F(SslStreamTest, HandshakeErrServer) {
  unsigned long cl_err = 0, srv_err = 0;

  auto client_fb = fibers::fiber([&] {
    cl_err = RunPeer(client_opts_, client_handshake_, client_engine_.get(), server_engine_.get());
  });

  srv_opts_.mutate_indx = 100;
  srv_opts_.mutate_val = 'R';

  auto server_fb = fibers::fiber([&] {
    srv_err = RunPeer(srv_opts_, srv_handshake_, server_engine_.get(), client_engine_.get());
  });

  client_fb.join();
  server_fb.join();

  LOG(INFO) << SSLError(cl_err);
  LOG(INFO) << SSLError(srv_err);

  ASSERT_NE(0, cl_err);
}

TEST_F(SslStreamTest, ReadShutdown) {
  unsigned long cl_err = 0, srv_err = 0;

  auto client_fb = fibers::fiber([&] {
    cl_err = RunPeer(client_opts_, client_handshake_, client_engine_.get(), server_engine_.get());
  });

  auto server_fb = fibers::fiber([&] {
    srv_err = RunPeer(srv_opts_, srv_handshake_, server_engine_.get(), client_engine_.get());
  });

  client_fb.join();
  server_fb.join();

  ASSERT_EQ(0, cl_err);
  ASSERT_EQ(0, srv_err);

  client_fb = fibers::fiber([&] {
    cl_err = RunPeer(client_opts_, shutdown_op_, client_engine_.get(), server_engine_.get());
  });

  server_fb = fibers::fiber([&] {
    srv_err = RunPeer(srv_opts_, read_op_, server_engine_.get(), client_engine_.get());
  });

  client_fb.join();
  server_fb.join();
}

}  // namespace tls

}  // namespace util
