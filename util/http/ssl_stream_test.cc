// Copyright 2019, Ubimo.com . All rights reserved.
// Author: Roman Gershman (roman@ubimo.com)
//
#include <boost/asio/ssl/error.hpp>

#include "base/gtest.h"
#include "base/logging.h"
#include "util/http/ssl_stream.h"

namespace util {
namespace http {

class SslStreamTest : public testing::Test {
 protected:
  void SetUp() override {
  }

  void TearDown() {
  }
};

TEST_F(SslStreamTest, BIO_s_bio_err) {
  BIO* bio1 = BIO_new(BIO_s_bio());
  constexpr char kData[] = "ROMAN";
  ASSERT_EQ(0, ERR_get_error());
  EXPECT_EQ(-2, BIO_write(bio1, kData, sizeof(kData)));
  int e = ERR_get_error();
  EXPECT_NE(0, e);
  EXPECT_EQ(BIO_F_BIO_WRITE_INTERN, ERR_GET_FUNC(e));
  EXPECT_EQ(BIO_R_UNINITIALIZED, ERR_GET_REASON(e));
  BIO_free(bio1);
}

TEST_F(SslStreamTest, BIO_s_bio_ZeroCopy) {
  BIO *bio1, *bio2;
  EXPECT_EQ(1, BIO_new_bio_pair(&bio1, 0, &bio2, 0));

  char *buf1 = nullptr, *buf2 = nullptr;
  ssize_t write_size = BIO_nwrite0(bio1, &buf1);
  EXPECT_EQ(17 * 1024, write_size);
  memset(buf1, 'a', write_size);

  EXPECT_EQ(write_size, BIO_nwrite(bio1, nullptr, write_size));  // commit.
  EXPECT_EQ(-1, BIO_nwrite(bio1, nullptr, 1));  // No space to commit.

  EXPECT_EQ(0, BIO_ctrl_get_write_guarantee(bio1));

  ASSERT_EQ(write_size, BIO_nread0(bio2, &buf2));
  EXPECT_EQ(0, memcmp(buf1, buf2, write_size));
  EXPECT_EQ(write_size, BIO_nread(bio2, nullptr, write_size));

  // bio1 is empty again.
  EXPECT_EQ(write_size, BIO_ctrl_get_write_guarantee(bio1));

  BIO_free(bio1);
  BIO_free(bio2);
}

}  // namespace http

}  // namespace util
