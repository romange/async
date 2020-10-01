// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>

#include "base/histogram.h"
#include "base/init.h"
#include "util/asio_stream_adapter.h"
#include "util/uring/accept_server.h"
#include "util/uring/fiber_socket.h"
#include "util/uring/http_handler.h"
#include "util/uring/proactor_pool.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/uring/varz.h"

using namespace boost;
using namespace std;
using namespace util;
using uring::FiberSocket;
using uring::Proactor;
using uring::ProactorPool;
using uring::SubmitEntry;
using tcp = asio::ip::tcp;

using IoResult = Proactor::IoResult;

DEFINE_int32(http_port, 8080, "Http port.");
DEFINE_int32(port, 8081, "Redis port");
DEFINE_uint32(n, 1000, "Number of requests per connection");
DEFINE_uint32(c, 10, "Number of connections per thread");
DEFINE_uint32(size, 8, "Message size");
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

class Driver {
  FiberSocket sock_;

  Driver(const Driver&) = delete;
 public:
  Driver(const tcp::endpoint& ep, Proactor* p) : sock_(p) {
    auto ec = sock_.Connect(ep);
    CHECK(!ec) << ec;
  }

  void Run(base::Histogram* dest);

};

void Driver::Run(base::Histogram* dest) {
  iovec vec[2];
  uint8_t buf[4];
  absl::little_endian::Store32(buf, FLAGS_size);
  std::unique_ptr<uint8_t[]> msg(new uint8_t[FLAGS_size]);
  vec[0].iov_base = buf;
  vec[0].iov_len = 4;
  vec[1].iov_base = msg.get();
  vec[1].iov_len = FLAGS_size;
  base::Histogram hist;

  for (unsigned msg = 0; msg < FLAGS_n; ++msg) {
    auto start = absl::GetCurrentTimeNanos();
    auto res = sock_.Send(vec, 2);
    CHECK(res.has_value()) << res.error();
    CHECK_EQ(res.value(), FLAGS_size + 4);

    auto res2 = sock_.Recv(vec, 2);
    CHECK(res.has_value()) << res.error();
    CHECK_EQ(res2.value(), FLAGS_size + 4);

    uint64_t dur = absl::GetCurrentTimeNanos() - start;
    hist.Add(dur / 1000);
  }
  dest->Merge(hist);
}

mutex lat_mu;
base::Histogram lat_hist;

void RunClient(tcp::endpoint ep, Proactor* p) {
  vector<fibers::fiber> drivers(FLAGS_c);
  base::Histogram hist;
  for (size_t i = 0; i < drivers.size(); ++i) {
    drivers[i] = fibers::fiber([&] {
      Driver d{ep, p};
      d.Run(&hist);
    });
  }

  for (size_t i = 0; i < drivers.size(); ++i) {
    drivers[i].join();
  }
  unique_lock<mutex> lk(lat_mu);
  lat_hist.Merge(hist);
}

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  CHECK_GT(FLAGS_port, 0);

  ProactorPool pp;
  pp.Run();

  if (FLAGS_connect.empty()) {
    RunServer(&pp);
  } else {
    asio::io_context io_context;
    tcp::resolver resolver{io_context};
    system::error_code ec;
    auto const results = resolver.resolve(FLAGS_connect, absl::StrCat(FLAGS_port), ec);
    CHECK(!ec) << "Could not resolve " << FLAGS_connect << " " << ec.message();
    CHECK(!results.empty());
    auto start = absl::GetCurrentTimeNanos();
    pp.AwaitFiberOnAll([&](Proactor* p) { RunClient(*results.begin(), p); });
    auto dur = absl::GetCurrentTimeNanos() - start;
    auto dur_ms = dur / 1000000;
    CONSOLE_INFO << "Total time " << dur_ms << " ms, average qps: "
                 << (pp.size() * size_t(FLAGS_c) * FLAGS_n) / (dur_ms / 1000) << "\n";
    CONSOLE_INFO << "Overall latency (usec) " << lat_hist.ToString();
  }
  pp.Stop();



  return 0;
}
