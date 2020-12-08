// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

// clang-format off
#include <sys/time.h>

#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
// clang-format on

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>

#include "base/histogram.h"
#include "base/init.h"
#include "util/asio_stream_adapter.h"
#include "util/accept_server.h"
#include "util/uring/fiber_socket.h"
#include "util/http_handler.h"
#include "util/uring/uring_pool.h"
#include "util/epoll/ev_pool.h"

#include "util/uring/uring_fiber_algo.h"
#include "util/varz.h"

using namespace boost;
using namespace std;
using namespace util;
using uring::FiberSocket;
using uring::Proactor;
using uring::SubmitEntry;
using uring::UringPool;
using tcp = asio::ip::tcp;

using IoResult = Proactor::IoResult;

DEFINE_bool(epoll, false, "If true use epoll for server");
DEFINE_int32(http_port, 8080, "Http port.");
DEFINE_int32(port, 8081, "Echo server port");
DEFINE_uint32(n, 1000, "Number of requests per connection");
DEFINE_uint32(c, 10, "Number of connections per thread");
DEFINE_uint32(size, 1, "Message size, 0 for hardcoded 4 byte pings");
DEFINE_string(connect, "", "hostname or ip address to connect to in client mode");

VarzQps ping_qps("ping-qps");

class EchoConnection : public Connection {
 public:
  EchoConnection() {
  }

 private:
  void HandleRequests() final;
  system::error_code ReadMsg(size_t* sz);

  std::unique_ptr<uint8_t[]> work_buf_;
  size_t req_len_ = 0;
};

system::error_code EchoConnection::ReadMsg(size_t* sz) {
  system::error_code ec;
  AsioStreamAdapter<FiberSocketBase> asa(*socket_);

  size_t bs = asio::read(asa, asio::buffer(work_buf_.get(), req_len_), ec);
  CHECK(ec || bs == req_len_);

  *sz = bs;
  return ec;
}

void EchoConnection::HandleRequests() {
  system::error_code ec;
  size_t sz;
  iovec vec[2];
  uint8_t buf[8];

  AsioStreamAdapter<FiberSocketBase> asa(*socket_);
  size_t bs = asio::read(asa, asio::buffer(buf), ec);
  CHECK(!ec && bs == sizeof(buf));
  req_len_ = absl::little_endian::Load64(buf);

  CHECK_LE(req_len_, 1UL << 18);
  work_buf_.reset(new uint8_t[req_len_]);

  while (true) {
    ec = ReadMsg(&sz);
    if (FiberSocket::IsConnClosed(ec))
      break;
    CHECK(!ec) << ec;
    ping_qps.Inc();

    vec[0].iov_base = buf;
    vec[0].iov_len = 4;
    absl::little_endian::Store32(buf, sz);
    vec[1].iov_base = work_buf_.get();
    vec[1].iov_len = sz;
    auto res = socket_->Send(vec, 2);
    CHECK(res.has_value());
  }
}

class EchoListener : public ListenerInterface {
 public:
  virtual Connection* NewConnection(ProactorBase* context) final {
    return new EchoConnection;
  }
};

void RunServer(ProactorPool* pp) {
  ping_qps.Init(pp);

  AcceptServer acceptor(pp);
  acceptor.AddListener(FLAGS_port, new EchoListener);
  if (FLAGS_http_port >= 0) {
    uint16_t port = acceptor.AddListener(FLAGS_http_port, new HttpListener<>);
    LOG(INFO) << "Started http server on port " << port;
  }

  acceptor.Run();
  acceptor.Wait();
}

class Driver {
  FiberSocket socket_;

  Driver(const Driver&) = delete;

 public:
  Driver(const tcp::endpoint& ep, Proactor* p);

  void Run(base::Histogram* dest);

 private:

  uint8_t buf_[8];
};

Driver::Driver(const tcp::endpoint& ep, Proactor* p) : socket_(p) {
  auto ec = socket_.Connect(ep);
  CHECK(!ec) << ec;
  VLOG(1) << "Connected to " << socket_.RemoteEndpoint();
}

void Driver::Run(base::Histogram* dest) {
  base::Histogram hist;

  absl::little_endian::Store64(buf_, FLAGS_size);
  std::unique_ptr<uint8_t[]> msg(new uint8_t[FLAGS_size]);

  uring::FiberSocket::expected_size_t es = socket_.Send(asio::buffer(buf_));
  CHECK_EQ(8U, es.value_or(0));  // Send expected payload size.

  iovec vec[2];
  vec[0].iov_len = 4;
  vec[0].iov_base = buf_;
  vec[1].iov_len = FLAGS_size;
  vec[1].iov_base = msg.get();

  for (unsigned i = 0; i < FLAGS_n; ++i) {
    auto start = absl::GetCurrentTimeNanos();
    es = socket_.Send(asio::buffer(msg.get(), FLAGS_size));
    CHECK(es.has_value()) << es.error();
    CHECK_EQ(es.value(), FLAGS_size);

    auto res2 = socket_.Recv(vec, 2);
    CHECK(res2.has_value()) << res2.error();
    CHECK_EQ(res2.value(), size_t(FLAGS_size + 4));

    uint64_t dur = absl::GetCurrentTimeNanos() - start;
    hist.Add(dur / 1000);
  }
  socket_.Shutdown(SHUT_RDWR);
  dest->Merge(hist);
}

mutex lat_mu;
base::Histogram lat_hist;

void RunClient(tcp::endpoint ep, ProactorBase* p) {
  vector<fibers::fiber> drivers(FLAGS_c);
  base::Histogram hist;
  for (size_t i = 0; i < drivers.size(); ++i) {
    drivers[i] = fibers::fiber([&] {
      Driver d{ep, static_cast<Proactor*>(p)};
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

  std::unique_ptr<ProactorPool> pp;

  if (FLAGS_connect.empty()) {
    if (FLAGS_epoll) {
      pp.reset(new epoll::EvPool);
    } else {
      pp.reset(new UringPool);
    }
    pp->Run();
    RunServer(pp.get());
  } else {
    CHECK_GT(FLAGS_size, 0U);

    pp.reset(new UringPool);
    pp->Run();

    // the simplest way to support dns resolution is to use asio resolver.
    asio::io_context io_context;
    tcp::resolver resolver{io_context};
    system::error_code ec;
    auto const results = resolver.resolve(FLAGS_connect, absl::StrCat(FLAGS_port), ec);
    CHECK(!ec) << "Could not resolve " << FLAGS_connect << " " << ec.message();
    CHECK(!results.empty());
    auto start = absl::GetCurrentTimeNanos();
    pp->AwaitFiberOnAll([&](auto* p) { RunClient(*results.begin(), p); });
    auto dur = absl::GetCurrentTimeNanos() - start;
    size_t dur_ms = std::max<size_t>(1, dur / 1000000);
    size_t dur_sec = std::max<size_t>(1, dur_ms / 1000);

    CONSOLE_INFO << "Total time " << dur_ms
                 << " ms, average qps: " << (pp->size() * size_t(FLAGS_c) * FLAGS_n) / dur_sec
                 << "\n";
    CONSOLE_INFO << "Overall latency (usec) " << lat_hist.ToString();
  }
  pp->Stop();

  return 0;
}
