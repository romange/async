// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/proactor.h"

#include <fcntl.h>
#include <gmock/gmock.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "absl/time/clock.h"
#include "base/gtest.h"
#include "base/logging.h"
#include "util/fibers/fibers_ext.h"
#include "util/accept_server.h"
#include "util/uring/uring_pool.h"
#include "util/uring/sliding_counter.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/uring/varz.h"

using namespace boost;
using namespace std;
using base::VarzValue;
using testing::ElementsAre;
using testing::Pair;

namespace util {
namespace uring {

constexpr uint32_t kRingDepth = 16;

class ProactorTest : public testing::Test {
 protected:
  void SetUp() override {
    proactor_ = std::make_unique<Proactor>();
    proactor_thread_ = thread{[this] {
      proactor_->Init(kRingDepth);
      proactor_->Run();
    }};
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
    this_fiber::sleep_for(20ms);
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

TEST_F(ProactorTest, SqPoll) {
  if (geteuid() != 0) {
    LOG(INFO) << "Requires root permissions";
    return;
  }

  io_uring_params params;
  memset(&params, 0, sizeof(params));

  // IORING_SETUP_SQPOLL require CAP_SYS_ADMIN permissions (root). In addition, every fd used
  // with this ring should be registered via io_uring_register syscall.
  params.flags |= IORING_SETUP_SQPOLL;
  io_uring ring;

  ASSERT_EQ(0, io_uring_queue_init_params(16, &ring, &params));
  io_uring_sqe* sqe = io_uring_get_sqe(&ring);
  io_uring_prep_nop(sqe);
  sqe->user_data = 42;
  int num_submitted = io_uring_submit(&ring);
  ASSERT_EQ(1, num_submitted);
  io_uring_cqe* cqe;

  ASSERT_EQ(0, io_uring_wait_cqe(&ring, &cqe));
  EXPECT_EQ(0, cqe->res);
  EXPECT_EQ(42, cqe->user_data);
  io_uring_cq_advance(&ring, 1);

  int fds[2];
  for (int i = 0; i < 2; ++i) {
    fds[i] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    ASSERT_GT(fds[i], 0);
  }
  int srv_fd = fds[0];
  int clientfd = fds[1];

  ASSERT_EQ(0, io_uring_register_files(&ring, fds, 2));

  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(10200);
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  int res = bind(srv_fd, (sockaddr*)&serv_addr, sizeof(serv_addr));
  ASSERT_EQ(0, res) << strerror(errno) << " " << errno;

  // Issue recv command
  sqe = io_uring_get_sqe(&ring);
  char buf[100];
  io_uring_prep_recv(sqe, 0, buf, sizeof(buf), 0);
  sqe->user_data = 43;
  sqe->flags |= IOSQE_FIXED_FILE;
  num_submitted = io_uring_submit(&ring);
  ASSERT_EQ(1, num_submitted);

  // Issue connect command. Cpnnect/accept are not supported by uring/sqpoll.
  res = connect(clientfd, (sockaddr*)&serv_addr, sizeof(serv_addr));
  ASSERT_EQ(0, res);
  write(clientfd, buf, 1);

  ASSERT_EQ(0, io_uring_wait_cqe_nr(&ring, &cqe, 1));
  ASSERT_EQ(43, cqe[0].user_data);
  EXPECT_EQ(1, cqe->res);  // Got 1 byte

  io_uring_queue_exit(&ring);
  close(srv_fd);
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
  UringPool pool{16, 2};
  pool.Run();

  pool.AwaitFiberOnAll([&](auto*) { val += 1; });
  EXPECT_EQ(2, val);
  pool.Stop();
}

TEST_F(ProactorTest, DispatchTest) {
  fibers::condition_variable cnd1, cnd2;
  fibers::mutex mu;
  int state = 0;

  LOG(INFO) << "LaunchFiber";
  auto fb = proactor_->LaunchFiber([&] {
    this_fiber::properties<FiberProps>().set_name("jessie");

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

  UringPool pool{16, 2};
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

  UringPool pool{16, 2};
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
