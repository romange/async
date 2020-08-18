// Copyright 2017, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <sys/time.h>

#include <string>

#include "base/integral_types.h"

typedef int64 MicrosecondsInt64;

// Time conversion utilities.
namespace base {

// Time conversion utilities.
static constexpr int64 kNumMillisPerSecond = 1000LL;

static constexpr int64 kNumMicrosPerMilli = 1000LL;
static constexpr int64 kNumMicrosPerSecond = kNumMicrosPerMilli * 1000LL;

inline MicrosecondsInt64 ToMicros(const timespec& ts) {
  return ts.tv_sec * kNumMicrosPerSecond + ts.tv_nsec / 1000;
}

template<clockid_t cid> inline MicrosecondsInt64 GetClockMicros() {
  timespec ts;
  clock_gettime(cid, &ts);
  return ToMicros(ts);
}

inline MicrosecondsInt64 GetThreadTime() {
  return base::GetClockMicros<CLOCK_THREAD_CPUTIME_ID>();
}

// Append result to a supplied string.
// If an error occurs during conversion 'dst' is not modified.
void StringAppendStrftime(std::string* dst,
                          const char* format,
                          time_t when,
                          bool local);

inline std::string LocalTimeNow(const char* format) {
  std::string result;
  StringAppendStrftime(&result, format, time(NULL), true);
  return result;
}

inline std::string PrintLocalTime(time_t seconds_epoch = time(NULL)) {
  std::string result;
  StringAppendStrftime(&result, "%d/%m/%Y %H:%M:%S %Z", seconds_epoch, true);
  return result;
}

}  // namespace base

// Returns the time since the Epoch measured in microseconds.
inline MicrosecondsInt64 GetCurrentTimeMicros() {
  return base::GetClockMicros<CLOCK_REALTIME>();
}

inline MicrosecondsInt64 GetMonotonicMicros() {
  return base::GetClockMicros<CLOCK_MONOTONIC>();
}


void SleepForMilliseconds(uint32 milliseconds);

namespace base {

// A timer and clock interface using posix CLOCK_MONOTONIC_COARSE clock).
class Timer {
  uint64 start_usec_;

public:
  static MicrosecondsInt64 Usec() {
    return GetClockMicros<CLOCK_MONOTONIC_COARSE>();
  }

  static MicrosecondsInt64 ResolutionUsec() {
    timespec ts;
    clock_getres(CLOCK_MONOTONIC_COARSE, &ts);

    return ToMicros(ts);
  }

  Timer() {
    start_usec_ = Usec();
  }

  uint64 EvalUsec() const { return Usec() - start_usec_; }
};


void SleepMicros(uint32 usec);


// Sets up 100usec precision fast timer.
void SetupJiffiesTimer();
void DestroyJiffiesTimer();

// Thread-safe. Very fast 100 microsecond precision monotonic clock.

constexpr uint32 kMicrosToJiffie = 100;
constexpr uint32 kJiffiesToMs = 10;

uint64 GetMonotonicJiffies();

uint64 GetMonotonicMicrosFast();

}  // namespace base

