// Copyright 2017, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#pragma once

#include <absl/base/optimization.h>

#include "base/integral_types.h"

// More info about popcnt:
// http://dalkescientific.com/writings/diary/popcnt.cpp
// https://github.com/kimwalisch/libpopcnt
class Bits {
 public:
  static int CountOnes(uint32 n) {
    return __builtin_popcount(n);
  }

  static inline constexpr int CountOnes64(uint64 n) {
    return __builtin_popcountll(n);
  }

  static int FindMSBSetNonZero(uint32 n) { return BsrNonZero(n); }
  static int FindMSBSet64NonZero(uint64 n) { return Bsr64NonZero(n); }

  // Return floor(log2(n)) for positive integer n.  Returns -1 iff n == 0.
  static int Log2Floor(uint32 n) {
    if (ABSL_PREDICT_FALSE(n == 0)) return -1;
    return BsrNonZero(n);
  }

  static int Log2Floor64(uint64 n) {
    if (ABSL_PREDICT_FALSE(n == 0)) return -1;
    return Bsr64NonZero(n);
  }


#if 0
  // Potentially faster version of Log2Floor() that returns an
  // undefined value if n == 0. 3 -> 1, 4 -> 2.
  // Slower than Bsr.
  static int Log2FloorNonZero(uint32 n) {
    return 31 ^ __builtin_clz(n);
  }

  // Slower than Bsr64.
  static int Log2FloorNonZero64(uint64 n) {
    return 63 ^ __builtin_clzll(n);
  }
#endif

  // Return the first set least significant bit, 0-indexed.  Returns an
  // undefined value if n == 0.  FindLSBSetNonZero() is similar to ffs() except
  // that it's 0-indexed.
  static constexpr int FindLSBSetNonZero(uint32 n) {
    return __builtin_ctz(n);
  }

  static constexpr int FindLSBSetNonZero64(uint64 n) {
    return __builtin_ctzll(n);
  }

  // Fast and straightforward versions with bsr operation.
  // Returns the last set (most significant) bit.
  // For n = 1, returns 0, for last MSB returns 63.
  // Undefined for 0 input.
  static constexpr uint64_t Bsr64NonZero(uint64_t n) {
    // We do not use ASM to allow multi-platform code
    return  63 ^ __builtin_clzll(n);
    /*uint64_t result;
    asm ("bsrq %1, %0" : "=r" (result) : "rm" (n));*/
  }

  // Returns MSB index (0-31). For 1 returns 0.
  static constexpr uint32_t BsrNonZero(uint32 v) {
    return 31 ^ __builtin_clz(v);
/*
   uint32_t result;
   asm("bsrl %1,%0" : "=r" (result) : "rm" (v));
   */
  }

  static constexpr unsigned CountLeadingZeros64(uint64_t val) {
    return __builtin_clzll(val);
  }

  // Returns smallest power of 2 greater or equal than x.
  // Fast cpu implementation is here:
  // https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
  // And here: http://www.hackersdelight.org/hdcode.htm
  // But we use bsr-based implementations as faster ones.
  // Based on https://locklessinc.com/articles/next_pow2/
  static uint32 RoundUp(uint32 x) {
    if (ABSL_PREDICT_FALSE(x <= 2))
      return x;

    int y = Bits::BsrNonZero(x - 1);
    return 2ULL << y;
  }

  static uint64 RoundUp64(uint64 x) {
    if (ABSL_PREDICT_FALSE(x <= 2))
      return x;

    int y = Bits::Bsr64NonZero(x - 1);
    return 2ULL << y;
  }

 private:
  Bits(const Bits&) = delete;
  void operator=(const Bits&) = delete;
};

static_assert(Bits::CountLeadingZeros64(1ULL << 63) == 0, "");
static_assert(Bits::CountLeadingZeros64(1ULL << 62) == 1, "");
static_assert(Bits::Bsr64NonZero(1ULL << 63) == 63, "");
static_assert(Bits::Bsr64NonZero(1ULL << 0) == 0, "");


namespace base {

template<typename T> uint32_t FindMsbNotZero(T val);
template<typename T> uint32_t FindLsbNotZero(T val);

template<> inline uint32_t FindMsbNotZero(uint32_t val) {
  return Bits::FindMSBSetNonZero(val);
}

template<> inline uint32_t FindMsbNotZero(uint64_t val) {
  return Bits::FindMSBSet64NonZero(val);
}

template<> inline uint32_t FindLsbNotZero(uint32_t val) {
  return Bits::FindLSBSetNonZero(val);
}

template<> inline uint32_t FindLsbNotZero(uint64_t val) {
  return Bits::FindLSBSetNonZero64(val);
}

extern const uint64_t powers_of_10_internal[];

inline unsigned CountDecimalDigit64(uint64_t n) {
  unsigned long i = Bits::Bsr64NonZero(n | 1);
  uint32_t t = (i + 1) * 1233 >> 12;
  return t - (n < powers_of_10_internal[t]) + 1;
#if 0
  // Simple pure C++ implementation
  if (n < 10) return 1;  if (n < 100) return 2;
  if (n < 1000) return 3; if (n < 10000) return 4;
  if (n < 100000) return 5; if (n < 1000000) return 6;
  if (n < 10000000) return 7; if (n < 100000000) return 8;
  if (n < 1000000000) return 9; if (n < 10000000000) return 10;
  if (n < 100000000000) return 11; if (n < 1000000000000) return 12;
  if (n < 10000000000000) return 13; if (n < 100000000000000) return 14;
  if (n < 1000000000000000) return 15; if (n < 10000000000000000) return 16;
  if (n < 100000000000000000) return 17; if (n < 1000000000000000000) return 18;
  if (n < 10000000000000000000) return 19;
  return 20;
#endif
}

// Returns a number between 1 and 10.
inline unsigned CountDecimalDigit32(uint32_t n) {
  unsigned long i = Bits::BsrNonZero(n | 1);
  uint32_t t = (i + 1) * 1233 >> 12;
  return t - (n < powers_of_10_internal[t]) + 1;
}


// n must be less or equal to 20.
inline uint64_t Power10(unsigned n) {
  return n == 0 ? 1 : powers_of_10_internal[n];
}

}  // namespace base
