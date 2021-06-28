// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include <mimalloc.h>
#include <jemalloc/jemalloc.h>
#include "base/gtest.h"
#include "base/logging.h"

namespace base {

using namespace std;

typedef struct dictEntry {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next;
} dictEntry;

static_assert(sizeof(dictEntry) == 24, "");

class MallocTest : public testing::Test {
 public:
};

TEST_F(MallocTest, Basic) {
  EXPECT_EQ(32, mi_good_size(24));

  // when jemalloc is configured --with-lg-quantum=3 it produces tight allocations.
  EXPECT_EQ(32, je_nallocx(24, 0));

  EXPECT_EQ(8, mi_good_size(5));
  EXPECT_EQ(8, je_nallocx(5, 0));

  EXPECT_EQ(16384, mi_good_size(16136));
  EXPECT_EQ(16384, mi_good_size(15240));
  EXPECT_EQ(14336, mi_good_size(13064));
  EXPECT_EQ(20480, mi_good_size(17288));
  EXPECT_EQ(32768, mi_good_size(28816));
}

}  // namespace base
