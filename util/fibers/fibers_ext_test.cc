// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "base/gtest.h"
#include "base/walltime.h"

#include "util/asio/io_context_pool.h"
#include "util/fibers/fiberqueue_threadpool.h"
#include "util/fibers/simple_channel.h"

using namespace boost;

namespace util {
namespace fibers_ext {

class FibersTest : public testing::Test {
 protected:
  void SetUp() final {}

  void TearDown() final {}

  static void SetUpTestCase() {
    base::SetupJiffiesTimer();
  }
};

TEST_F(FibersTest, SimpleChannel) {
  SimpleChannel<int> channel(10);
  ASSERT_TRUE(channel.TryPush(2));
  channel.Push(4);

  int val = 0;
  ASSERT_TRUE(channel.Pop(val));
  EXPECT_EQ(2, val);
  ASSERT_TRUE(channel.Pop(val));
  EXPECT_EQ(4, val);

  fibers::fiber fb(fibers::launch::post, [&] { EXPECT_TRUE(channel.Pop(val)); });
  channel.Push(7);
  fb.join();
  EXPECT_EQ(7, val);

  fb = fibers::fiber(fibers::launch::post, [&] { EXPECT_FALSE(channel.Pop(val)); });
  channel.StartClosing();
  fb.join();
}

TEST_F(FibersTest, EventCount) {
  EventCount ec;
  bool signal = false;
  bool fb_exit = false;

  fibers::fiber fb(fibers::launch::dispatch, [&] {
    ec.await([&] { return signal; });
    fb_exit = true;
  });
  ec.notify();
  this_fiber::yield();
  EXPECT_FALSE(fb_exit);

  signal = true;
  ec.notify();
  fb.join();
}

TEST_F(FibersTest, SpuriousNotify) {
  EventCount ec;

  ASSERT_FALSE(ec.notify());
  ASSERT_FALSE(ec.notifyAll());

  int val = 0;

  auto check_positive = [&val]() -> bool { return val > 0; };
  std::thread t1([check_positive, &ec]() { ec.await(check_positive); });

  while (!ec.notify())
    SleepForMilliseconds(1);
  val = 1;
  ASSERT_TRUE(ec.notify());
  t1.join();

  ASSERT_FALSE(ec.notify());
}

TEST_F(FibersTest, FQTP) {
  FiberQueueThreadPool pool(1, 2);

  for (unsigned i = 0; i < 10000; ++i) {
    ASSERT_EQ(i, pool.Await([=] {
      sched_yield();
      return i;
    }));
  }
}

TEST_F(FibersTest, SimpleChannelDone) {
  SimpleChannel<std::function<void()>> s(2);
  std::thread t([&] {
    while (true) {
      std::function<void()> f;
      if (!s.Pop(f))
        break;
      f();
    }
  });

  for (unsigned i = 0; i < 100; ++i) {
    Done done;
    s.Push([done] () mutable { done.Notify();});
    done.Wait();
  }
  s.StartClosing();
  t.join();
}

TEST_F(FibersTest, FiberQueue) {
  IoContextPool pool{1};
  pool.Run();

  IoContext& cntx = pool.GetNextContext();
  FiberQueue fq{32};

  auto fiber = cntx.LaunchFiber([&] {
    this_fiber::properties<IoFiberProperties>().SetNiceLevel(1);
    fq.Run();
  });

  constexpr unsigned kIters = 10000;
  size_t delay = 0;
  size_t invocations = 0;
  for (unsigned i = 0; i < kIters; ++i) {
    auto start = base::GetMonotonicMicrosFast();
    ASSERT_GT(start, 0);

    fq.Add([&, start] {
      ASSERT_TRUE(cntx.InContextThread());
      delay += base::GetMonotonicMicrosFast() - start;
      ++invocations;
    });
  }
  fq.Shutdown();
  fiber.join();

  EXPECT_EQ(kIters, invocations);
  EXPECT_LT(delay / kIters, 2000);  //
  EXPECT_GT(delay, 0);  //
}

}  // namespace fibers_ext
}  // namespace util
