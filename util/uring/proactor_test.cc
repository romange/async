// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/proactor.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <gmock/gmock.h>

#include "base/gtest.h"
#include "base/logging.h"
#include "util/fibers/fibers_ext.h"
#include "util/uring/accept_server.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/uring/proactor_pool.h"
#include "util/uring/sliding_counter.h"
#include "util/uring/varz.h"

using namespace boost;
using namespace std;
using testing::Pair;
using testing::ElementsAre;

namespace util {
namespace uring {

constexpr uint32_t kRingDepth = 16;

class ProactorTest : public testing::Test {
 protected:
  void SetUp() override {
    proactor_ = std::make_unique<Proactor>();
    proactor_thread_ = thread{[this] { proactor_->Run(kRingDepth); }};
  }

  void TearDown() {
    proactor_->Stop();
    proactor_thread_.join();
    proactor_.reset();
  }

  static void SetUpTestCase() {
    testing::FLAGS_gtest_death_test_style="threadsafe";
  }

  using IoResult = Proactor::IoResult;

  std::unique_ptr<Proactor> proactor_;
  std::thread proactor_thread_;
};

TEST_F(ProactorTest, AsyncCall) {
  for (unsigned i = 0; i < 10000; ++i) {
    proactor_->AsyncBrief([] {});
  }
  usleep(5000);
}

TEST_F(ProactorTest, Await) {
  thread_local int val = 5;

  proactor_->AwaitBrief([] { val = 15; });
  EXPECT_EQ(5, val);

  int j = proactor_->AwaitBrief([] { return val; });
  EXPECT_EQ(15, j);
}

TEST_F(ProactorTest, Sleep) {
  proactor_->AwaitBlocking([] {
    LOG(INFO) << "Before Sleep";
    this_fiber::sleep_for(10ms);
    LOG(INFO) << "After Sleep";
  });
}

TEST_F(ProactorTest, SqeOverflow) {
  size_t unique_id = 0;
  char buf[128];

  int fd = open(gflags::GetArgv0(), O_RDONLY | O_CLOEXEC);
  CHECK_GT(fd, 0);

  constexpr size_t kMaxPending = kRingDepth * 100;
  fibers_ext::BlockingCounter bc(kMaxPending);
  auto cb = [&bc](IoResult, int64_t payload, Proactor*) {
    bc.Dec();
  };

  proactor_->AsyncFiber([&]() mutable {
    for (unsigned i = 0; i < kMaxPending; ++i) {
      SubmitEntry se = proactor_->GetSubmitEntry(cb, unique_id++);
      se.PrepRead(fd, buf, sizeof(buf), 0);
      VLOG(1) << i;
    }
  });

  bc.Wait();  // We wait on all callbacks before closing FD that is being handled by IO loop.

  close(fd);
}

TEST_F(ProactorTest, AsyncEvent) {
  fibers_ext::Done done;

  auto cb = [done](IoResult, int64_t payload, Proactor* p) mutable {
    done.Notify();
  };

  proactor_->AsyncBrief([&] {
    SubmitEntry se = proactor_->GetSubmitEntry(std::move(cb), 1);
    se.sqe()->opcode = IORING_OP_NOP;
  });
  done.Wait();
}

TEST_F(ProactorTest, Pool) {
  std::atomic_int val{0};
  ProactorPool pool{2};
  pool.Run();

  pool.AwaitFiberOnAll([&](Proactor*) { val +=1; });
  EXPECT_EQ(2, val);
  pool.Stop();
}

TEST_F(ProactorTest, DispatchTest) {
  fibers::condition_variable cnd1, cnd2;
  fibers::mutex mu;
  int state = 0;

  LOG(INFO) << "LaunchFiber";
  auto fb = proactor_->LaunchFiber([&] {
    this_fiber::properties<UringFiberProps>().set_name("jessie");

    std::unique_lock<fibers::mutex> g(mu);
    state = 1;
    LOG(INFO) << "state 1";

    cnd2.notify_one();
    cnd1.wait(g, [&] { return state == 2;});
    LOG(INFO) << "End";
  });

  {
    std::unique_lock<fibers::mutex> g(mu);
    cnd2.wait(g, [&] { return state == 1;});
    state = 2;
    LOG(INFO) << "state 2";
    cnd1.notify_one();
  }
  LOG(INFO) << "BeforeJoin";
  fb.join();
}

TEST_F(ProactorTest, SlidingCounter) {
  SlidingCounterTL<10> sc;
  proactor_->AwaitBrief([&] { sc.Inc();});
  uint32_t cnt = proactor_->AwaitBrief([&] { return sc.Sum();});
  EXPECT_EQ(1, cnt);

  ProactorPool pool{2};
  pool.Run();
  SlidingCounter<4> sc2;
  sc2.Init(&pool);
  pool.AwaitOnAll([&](auto*) {sc2.Inc();});
  cnt = sc2.Sum();
  EXPECT_EQ(2, cnt);
}

TEST_F(ProactorTest, Varz) {
  VarzQps qps("test1");
  ASSERT_DEATH(qps.Inc(), "");

  ProactorPool pool{2};
  pool.Run();

  qps.Init(&pool);

  vector<pair<string, uint32_t>> vals;
  auto cb =[&vals](const char* str, VarzValue&& v) {
    vals.emplace_back(str, v.num);
  };
  pool[0].AwaitBlocking([&] { qps.Iterate(cb);} );

  EXPECT_THAT(vals, ElementsAre(Pair("test1", 0)));
}


void BM_AsyncCall(benchmark::State& state) {
  Proactor proactor;
  std::thread t([&] { proactor.Run(); });

  while (state.KeepRunning()) {
    proactor.AsyncBrief([] {});
  }
  proactor.Stop();
  t.join();
}
BENCHMARK(BM_AsyncCall);

void BM_AwaitCall(benchmark::State& state) {
  Proactor proactor;
  std::thread t([&] { proactor.Run(); });

  while (state.KeepRunning()) {
    proactor.AwaitBrief([] {});
  }
  proactor.Stop();
  t.join();
}
BENCHMARK(BM_AwaitCall);

}  // namespace uring
}  // namespace util
