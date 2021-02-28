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
  EXPECT_EQ(32, je_nallocx(24, 0));
}

}  // namespace base
