// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/proactor.h"

#include <fcntl.h>
#include <gmock/gmock.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "base/gtest.h"
#include "base/logging.h"
#include "util/fibers/fibers_ext.h"
#include "util/uring/accept_server.h"
#include "util/uring/proactor_pool.h"
#include "util/uring/sliding_counter.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/uring/varz.h"

using namespace boost;
using namespace std;
using testing::ElementsAre;
using testing::Pair;
using base::VarzValue;

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
    testing::FLAGS_gtest_death_test_style = "threadsafe";
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
  auto cb = [&bc](IoResult, int64_t payload, Proactor*) { bc.Dec(); };

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

static void InitSqe(int op, int fd, io_uring_sqe* sqe) {
  sqe->opcode = op;
  sqe->fd = fd;
  sqe->flags = 0;
  sqe->ioprio = 0;
  sqe->addr = 0;
  sqe->off = 0;
  sqe->len = 0;
  sqe->rw_flags = 0;
  sqe->__pad2[0] = sqe->__pad2[1] = sqe->__pad2[2] = 0;
}

TEST_F(ProactorTest, SqPoll) {
  if (geteuid() != 0) {
    LOG(INFO) << "Requires root permissions";
    return;
  }

  io_uring_params params;
  memset(&params, 0, sizeof(params));

  // IORING_SETUP_SQPOLL require special permissions. In addition, every fd used
  // with this ring should be registered via io_uring_register syscall.
  params.flags |= IORING_SETUP_SQPOLL;
  io_uring ring;

  ASSERT_EQ(0, io_uring_queue_init_params(16, &ring, &params));
  io_uring_sqe* sqe = io_uring_get_sqe(&ring);
  InitSqe(IORING_OP_NOP, -1, sqe);
  sqe->user_data = 42;
  int num_submitted = io_uring_submit(&ring);
  ASSERT_EQ(1, num_submitted);
  io_uring_cqe* cqe;

  ASSERT_EQ(0, io_uring_wait_cqe(&ring, &cqe));
  EXPECT_EQ(0, cqe->res);
  EXPECT_EQ(42, cqe->user_data);
  io_uring_cq_advance(&ring, 1);

  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(1234);
  CHECK_EQ(1, inet_aton("127.0.0.1", &serv_addr.sin_addr));

  sqe = io_uring_get_sqe(&ring);
  InitSqe(IORING_OP_CONNECT, fd, sqe);
  sqe->addr = (unsigned long)&serv_addr;
  sqe->len = 0;
  sqe->off = sizeof(serv_addr);
  sqe->user_data = 43;
  num_submitted = io_uring_submit(&ring);
  ASSERT_EQ(1, num_submitted);
  ASSERT_EQ(0, io_uring_wait_cqe(&ring, &cqe));
  EXPECT_EQ(-EBADF, cqe->res);  // Due to the fact we did not register fd.
  EXPECT_EQ(43, cqe->user_data);

  io_uring_queue_exit(&ring);
  close(fd);
}

TEST_F(ProactorTest, AsyncEvent) {
  fibers_ext::Done done;

  auto cb = [done](IoResult, int64_t payload, Proactor* p) mutable { done.Notify(); };

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

  pool.AwaitFiberOnAll([&](Proactor*) { val += 1; });
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
    cnd1.wait(g, [&] { return state == 2; });
    LOG(INFO) << "End";
  });

  {
    std::unique_lock<fibers::mutex> g(mu);
    cnd2.wait(g, [&] { return state == 1; });
    state = 2;
    LOG(INFO) << "state 2";
    cnd1.notify_one();
  }
  LOG(INFO) << "BeforeJoin";
  fb.join();
}

TEST_F(ProactorTest, SlidingCounter) {
  SlidingCounterTL<10> sc;
  proactor_->AwaitBrief([&] { sc.Inc(); });
  uint32_t cnt = proactor_->AwaitBrief([&] { return sc.Sum(); });
  EXPECT_EQ(1, cnt);

  ProactorPool pool{2};
  pool.Run();
  SlidingCounter<4> sc2;
  sc2.Init(&pool);
  pool.AwaitOnAll([&](auto*) { sc2.Inc(); });
  cnt = sc2.Sum();
  EXPECT_EQ(2, cnt);
}

TEST_F(ProactorTest, Varz) {
  VarzQps qps("test1");
  ASSERT_DEATH(qps.Inc(), "");  // Messes up log-file softlink.

  ProactorPool pool{2};
  pool.Run();

  qps.Init(&pool);

  vector<pair<string, uint32_t>> vals;
  auto cb = [&vals](const char* str, VarzValue&& v) { vals.emplace_back(str, v.num); };
  pool[0].AwaitBlocking([&] { qps.Iterate(cb); });

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
