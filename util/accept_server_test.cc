// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/accept_server.h"

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include "base/gtest.h"
#include "base/logging.h"
#include "util/asio_stream_adapter.h"
#include "util/listener_interface.h"
#include "util/uring/fiber_socket.h"
#include "util/uring/uring_pool.h"
#include "util/epoll/ev_pool.h"

namespace util {
namespace uring {

using namespace boost;
using namespace std;
namespace h2 = beast::http;
using fibers_ext::BlockingCounter;

class TestConnection : public Connection {
 protected:
  void HandleRequests() final;
};

void TestConnection::HandleRequests() {
  char buf[128];
  system::error_code ec;

  AsioStreamAdapter<FiberSocketBase> asa(*socket_);

  while (true) {
    asa.read_some(asio::buffer(buf), ec);
    if (ec == std::errc::connection_aborted)
      break;

    CHECK(!ec) << ec << "/" << ec.message();

    asa.write_some(asio::buffer(buf), ec);

    if (FiberSocket::IsConnClosed(ec))
      break;

    CHECK(!ec);
  }
  VLOG(1) << "TestConnection exit";
}

class TestListener : public ListenerInterface {
 public:
  virtual Connection* NewConnection(ProactorBase* context) final {
    return new TestConnection;
  }
};

class AcceptServerTest : public testing::Test {
 protected:
  void SetUp() override;

  void TearDown() {
    as_->Stop(true);
    pp_->Stop();
  }

  static void SetUpTestCase() {
  }

  using IoResult = Proactor::IoResult;

  std::unique_ptr<ProactorPool> pp_;
  std::unique_ptr<AcceptServer> as_;
  std::unique_ptr<FiberSocketBase> client_sock_;
};

void AcceptServerTest::SetUp() {
  const uint16_t kPort = 1234;
#ifdef USE_URING
  ProactorPool* up = new UringPool(16, 2);
#else
  ProactorPool* up = new epoll::EvPool(2);
#endif
  pp_.reset(up);
  pp_->Run();

  as_.reset(new AcceptServer{up});
  as_->AddListener(kPort, new TestListener);
  as_->Run();

  ProactorBase* pb = pp_->GetNextProactor();
  client_sock_.reset(pb->CreateSocket());
  auto address = asio::ip::make_address("127.0.0.1");
  asio::ip::tcp::endpoint ep{address, kPort};

  pb->AwaitBlocking([&] {
    FiberSocket::error_code ec = client_sock_->Connect(ep);
    CHECK(!ec) << ec;
  });
}

void RunClient(FiberSocketBase* fs, BlockingCounter* bc) {
  LOG(INFO) << ": Ping-client started";
  AsioStreamAdapter<> asa(*fs);

  ASSERT_TRUE(fs->IsOpen());

  h2::request<h2::string_body> req(h2::verb::get, "/", 11);
  req.body().assign("foo");
  req.prepare_payload();
  h2::write(asa, req);

  bc->Dec();

  LOG(INFO) << ": echo-client stopped";
}

TEST_F(AcceptServerTest, Basic) {
  fibers_ext::BlockingCounter bc(1);
  client_sock_->proactor()->AsyncFiber(&RunClient, client_sock_.get(), &bc);

  bc.Wait();
}

TEST_F(AcceptServerTest, Break) {
  usleep(1000);
  as_->Stop(true);
}

}  // namespace uring
}  // namespace util
