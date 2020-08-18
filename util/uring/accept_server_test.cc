// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/uring/accept_server.h"

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include "base/gtest.h"
#include "base/logging.h"
#include "util/asio_stream_adapter.h"
#include "util/uring/proactor_pool.h"

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

  AsioStreamAdapter<FiberSocket> asa(socket_);

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
  virtual Connection* NewConnection(Proactor* context) {
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
  FiberSocket client_sock_;
};

void AcceptServerTest::SetUp() {
  const uint16_t kPort = 1234;

  pp_.reset(new ProactorPool(2));
  pp_->Run(16);

  as_.reset(new AcceptServer{pp_.get()});
  as_->AddListener(kPort, new TestListener);
  as_->Run();

  client_sock_.set_proactor(pp_->GetNextProactor());
  auto address = asio::ip::make_address("127.0.0.1");
  asio::ip::tcp::endpoint ep{address, kPort};

  client_sock_.proactor()->AwaitBlocking([&] {
    FiberSocket::error_code ec = client_sock_.Connect(ep);
    CHECK(!ec) << ec;
  });
}

void RunClient(FiberSocket* fs, BlockingCounter* bc) {
  LOG(INFO) << ": Ping-client started";
  AsioStreamAdapter<FiberSocket> asa(*fs);

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
  client_sock_.proactor()->AsyncFiber(&RunClient, &client_sock_, &bc);

  bc.Wait();
}

TEST_F(AcceptServerTest, Break) {
  usleep(1000);
  as_->Stop(true);
}

}  // namespace uring
}  // namespace util
