// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include <boost/asio/read.hpp>

#include "base/init.h"
#include "util/asio_stream_adapter.h"
#include "util/uring/accept_server.h"
#include "util/uring/fiber_socket.h"
#include "util/uring/http_handler.h"
#include "util/uring/proactor_pool.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/uring/varz.h"

using namespace boost;
using namespace util;
using uring::FiberSocket;
using uring::Proactor;
using uring::ProactorPool;
using uring::SubmitEntry;

using IoResult = Proactor::IoResult;

DEFINE_int32(http_port, 8080, "Http port.");
DEFINE_int32(port, 8081, "Redis port");
DEFINE_string(connect, "", "hostname or ip address to connect to in client mode");

uring::VarzQps ping_qps("ping-qps");

class EchoConnection : public uring::Connection {
 public:
  EchoConnection() {
    work_buf_.reset(new uint8_t[1 << 16]);
  }

  void Handle(IoResult res, int32_t payload, Proactor* mgr);

 private:
  void HandleRequests() final;
  system::error_code ReadMsg(size_t* sz);

  std::unique_ptr<uint8_t[]> work_buf_;
};

system::error_code EchoConnection::ReadMsg(size_t* sz) {
  system::error_code ec;
  AsioStreamAdapter<FiberSocket> asa(socket_);

  uint8_t buf[4];
  size_t buf_sz = asio::read(asa, asio::buffer(buf), ec);
  if (ec)
    return ec;
  CHECK_EQ(buf_sz, 4u);
  buf_sz = absl::little_endian::Load32(buf);
  CHECK_LT(buf_sz, 1u << 16);
  size_t bs = asio::read(asa, asio::buffer(work_buf_.get(), buf_sz), ec);
  CHECK(ec || bs == buf_sz);

  *sz = buf_sz;
  return ec;
}

void EchoConnection::HandleRequests() {
  system::error_code ec;
  size_t sz;
  iovec vec[2];
  uint8_t buf[4];

  while (true) {
    ec = ReadMsg(&sz);
    if (FiberSocket::IsConnClosed(ec))
      break;
    CHECK(!ec) << ec;

    vec[0].iov_base = buf;
    vec[0].iov_len = 4;
    absl::little_endian::Store32(buf, sz);
    vec[1].iov_base = work_buf_.get();
    vec[1].iov_len = sz;
    auto res = socket_.Send(vec, 2);
    CHECK(res.has_value());
  }
  socket_.Shutdown(SHUT_RDWR);
}

class EchoListener : public uring::ListenerInterface {
 public:
  virtual uring::Connection* NewConnection(Proactor* context) {
    return new EchoConnection;
  }
};

void RunServer(ProactorPool* pp) {
  ping_qps.Init(pp);

  uring::AcceptServer uring_acceptor(pp);
  uring_acceptor.AddListener(FLAGS_port, new EchoListener);
  if (FLAGS_http_port >= 0) {
    uint16_t port = uring_acceptor.AddListener(FLAGS_http_port, new uring::HttpListener<>);
    LOG(INFO) << "Started http server on port " << port;
  }

  uring_acceptor.Run();
  uring_acceptor.Wait();
}

void RunClient(ProactorPool* pp) {
}

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  CHECK_GT(FLAGS_port, 0);

  ProactorPool pp;
  pp.Run();

  if (FLAGS_connect.empty()) {
    RunServer(&pp);
  } else {
    RunClient(&pp);
  }
  pp.Stop();

  return 0;
}
