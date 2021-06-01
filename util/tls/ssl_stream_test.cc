// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/tls/ssl_stream.h"

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

  void SetUp() override {
    client_engine_.reset(new detail::Engine(CreateSslCntx(true)));
    server_engine_.reset(new detail::Engine(CreateSslCntx(false)));
  }

  void TearDown() {
  }

  using OpCb = std::function<Engine::OpResult(Engine*)>;

  static void RunPeer(string name, OpCb cb, Engine* src, Engine* dest);

  static void PrintError(unsigned long);

  SSL_CTX* CreateSslCntx(bool is_client);

  unique_ptr<detail::Engine> client_engine_, server_engine_;
};

int verify_callback(int ok, X509_STORE_CTX* ctx) {
  LOG(WARNING) << "verify_callback: " << ok;
  return 1;
}

void SslStreamTest::PrintError(unsigned long e) {
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  LOG(FATAL) << "SSL Error: " << buf;
}

SSL_CTX* SslStreamTest::CreateSslCntx(bool is_client) {
  SSL_CTX* ctx = SSL_CTX_new(is_client ? TLS_client_method() : TLS_server_method());

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  SSL_CTX_set_options(ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);

  // SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, verify_callback);

  // Can also be defined using "ADH:@SECLEVEL=0" cipher string below.
  SSL_CTX_set_security_level(ctx, 0);
  if (is_client) {
  } else {
    SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_session_id_context(ctx, (const unsigned char*)"test", 4);

    CHECK_EQ(1, SSL_CTX_set_dh_auto(ctx, 1));
  }

  CHECK_EQ(1, SSL_CTX_set_cipher_list(ctx, "ADH"));

  return ctx;
}

void SslStreamTest::RunPeer(string name, OpCb cb, Engine* src, Engine* dest) {
  ERR_print_errors_fp(stderr);  // Empties the queue.

  while (true) {
    auto op_result = cb(src);
    if (!op_result) {
      PrintError(op_result.error());
    }
    LOG(INFO) << name << " OpResult: " << *op_result;
    if (src->OutputPending() > 0) {
      auto buf_result = src->GetOutputBuf();
      CHECK(buf_result);
      LOG(INFO) << name << " wrote " << buf_result->size() << " bytes";
      ASSERT_FALSE(buf_result->empty());

      auto write_result = dest->WriteBuf(*buf_result);
      ASSERT_TRUE(write_result);
      ASSERT_EQ(*write_result, buf_result->size());
    }
    if (*op_result > 0) {
      return;
    }
    ASSERT_EQ(-1, *op_result);
    this_fiber::yield();
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
  auto client_cb = [](Engine* eng) { return eng->Handshake(Engine::CLIENT); };
  auto client_fb =
      fibers::fiber(&RunPeer, "client", client_cb, client_engine_.get(), server_engine_.get());

  auto server_cb = [](Engine* eng) { return eng->Handshake(Engine::SERVER); };
  auto server_fb =
      fibers::fiber(&RunPeer, "server", server_cb, server_engine_.get(), client_engine_.get());

  client_fb.join();
  server_fb.join();
}

}  // namespace tls

}  // namespace util
