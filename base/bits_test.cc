// Copyright 2017, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
// To see assembler `objdump -S -M intel base/CMakeFiles/bits_test.dir/bits_test.cc.o`
// To compile assembler:
//   gcc -O3 -mtune=native -mavx -std=c++11 -S -masm=intel  -fverbose-asm bits_test.cc
//    -I.. -I../third_party/libs/benchmark/include/ -I../third_party/libs/gtest/include/

#include <absl/base/macros.h>

#include "base/bits.h"

#include "base/gtest.h"
#include "base/stl_util.h"

namespace base {

__attribute__ ((noinline)) static unsigned count_bits(uint32 val) {
    return Bits::CountOnes(val);
}

class BitsTest {

};

TEST(BitsTest, Basic) {
  uint64 nums[] = { 0,1, 1,2, 1,3, 2,7, 4,30, 63};
  for (unsigned i = 0; i < ABSL_ARRAYSIZE(nums)/2; i+=2) {
    ASSERT_EQ(nums[i], Bits::FindMSBSetNonZero(nums[i+1]));
    EXPECT_EQ(nums[i], Bits::Bsr64NonZero(nums[i+1]));
    EXPECT_EQ(nums[i], Bits::BsrNonZero(nums[i+1]));
    EXPECT_EQ(nums[i], Bits::FindMSBSetNonZero(nums[i+1]));
  }
  EXPECT_EQ(0, Bits::Bsr64NonZero(1));

  volatile unsigned val = 1 + 4 + 16 + 64;
  volatile unsigned val2 = count_bits(val);
  ASSERT_EQ(4, val2);

  EXPECT_EQ(0, Bits::RoundUp(0));
  EXPECT_EQ(1, Bits::RoundUp(1));
  EXPECT_EQ(8, Bits::RoundUp(5));
  EXPECT_EQ(8, Bits::RoundUp(8));

  EXPECT_EQ(1U << 31, Bits::RoundUp((1U << 31) - 1));
  EXPECT_EQ(1ULL << 63, Bits::RoundUp64((1ULL << 63) - 1));
  EXPECT_EQ(1ULL << 47, Bits::RoundUp64((1ULL << 47) - (1 << 16)));
}

TEST(BitsTest, Decimal) {
  EXPECT_EQ(2, CountDecimalDigit64(10));
  EXPECT_EQ(2, CountDecimalDigit32(10));

  EXPECT_EQ(2, CountDecimalDigit64(99));
  EXPECT_EQ(2, CountDecimalDigit32(99));

  EXPECT_EQ(3, CountDecimalDigit64(100));
  EXPECT_EQ(3, CountDecimalDigit32(100));

  EXPECT_EQ(7, CountDecimalDigit64(9999999));
  EXPECT_EQ(7, CountDecimalDigit32(9999999));

  for (unsigned i = 0; i < 10; ++i) {
    EXPECT_EQ(1, CountDecimalDigit64(i));
    EXPECT_EQ(1, CountDecimalDigit32(i));
  }
  EXPECT_EQ(1, Power10(0));
  EXPECT_EQ(10, Power10(1));
}

TEST(BitsTest, MinMax) {
  MinMax<int> mm;
  mm.Update(5);

  EXPECT_EQ(5, mm.min_val);
  EXPECT_EQ(5, mm.max_val);

  mm.Update(15);
  EXPECT_EQ(5, mm.min_val);
  EXPECT_EQ(15, mm.max_val);
}

using benchmark::DoNotOptimize;

static void BM_RoundUp64(benchmark::State& state) {
  volatile uint64 val[128];
  std::fill(val, val + ABSL_ARRAYSIZE(val), 1ULL << 47);

  while (state.KeepRunning()) {
    for (unsigned i = 0; i < ABSL_ARRAYSIZE(val); ++i) {
      DoNotOptimize(Bits::RoundUp64(val[i]));
    }
  }
}
BENCHMARK(BM_RoundUp64);

static void BM_RoundUp(benchmark::State& state) {
  volatile uint32 val[128];
  std::fill(val, val + ABSL_ARRAYSIZE(val), 1ULL << 47);

  while (state.KeepRunning()) {
    for (unsigned i = 0; i < ABSL_ARRAYSIZE(val); ++i) {
      DoNotOptimize(Bits::RoundUp(val[i]));
    }
  }
}
BENCHMARK(BM_RoundUp);

}  // namespace base
