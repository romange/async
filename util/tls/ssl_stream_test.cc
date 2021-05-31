// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/tls/ssl_stream.h"

#include <openssl/err.h>

#include "base/gtest.h"
#include "base/logging.h"

namespace util {
namespace tls {

class SslStreamTest : public testing::Test {
 protected:
  void SetUp() override {
  }

  void TearDown() {
  }
};

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

}  // namespace tls

}  // namespace util
