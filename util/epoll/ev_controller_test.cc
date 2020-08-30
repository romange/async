// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/epoll/ev_controller.h"

#include <fcntl.h>
#include <gmock/gmock.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "absl/time/clock.h"
#include "base/gtest.h"
#include "base/logging.h"
#include "base/varz_value.h"
#include "util/fibers/fibers_ext.h"
#include "util/epoll/epoll_fiber_scheduler.h"


using namespace boost;
using namespace std;
using testing::ElementsAre;
using testing::Pair;
using base::VarzValue;

namespace util {
namespace epoll {


class EvControllerTest : public testing::Test {
 protected:
  void SetUp() override {
    ev_cntrl_ = std::make_unique<EvController>();
    ev_cntrl_thread_ = thread{[this] { ev_cntrl_->Run(); }};
  }

  void TearDown() {
    ev_cntrl_->Stop();
    ev_cntrl_thread_.join();
    ev_cntrl_.reset();
  }

  static void SetUpTestCase() {
    testing::FLAGS_gtest_death_test_style = "threadsafe";
  }


  std::unique_ptr<EvController> ev_cntrl_;
  std::thread ev_cntrl_thread_;
};

TEST_F(EvControllerTest, AsyncCall) {
  for (unsigned i = 0; i < 10000; ++i) {
    ev_cntrl_->AsyncBrief([] {});
  }
  usleep(5000);
}

}  // namespace epoll
}  // namespace util
