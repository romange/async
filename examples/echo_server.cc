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
DEFINE_uint32(backlog, 1024, "Accept queue length");

DEFINE_string(connect, "", "hostname or ip address to connect to in client mode");

VarzQps ping_qps("ping-qps");
VarzCount connections("connections");

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

  int yes = 1;
  CHECK_EQ(0, setsockopt(socket_->native_handle(), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)));

  connections.IncBy(1);
  AsioStreamAdapter<FiberSocketBase> asa(*socket_);
  vec[0].iov_base = buf;
  vec[0].iov_len = 8;

  auto ep = socket_->RemoteEndpoint();
  LOG(INFO) << "Waiting for size from " << ep;
  auto es = socket_->Recv(asio::buffer(buf, 8));
  if (!es.has_value()) {
    if (es.error().value() == ECONNABORTED)
      return;
    LOG(FATAL) << "Bad Conn Handshake " << es.error() << " for socket " << ep;
  } else {
    CHECK_EQ(es.value(), 8U);
    LOG(INFO) << "Received size from " << ep;
    uint8_t val = 1;
    socket_->Send(asio::buffer(&val, 1));
  }

  // size_t bs = asio::read(asa, asio::buffer(buf), ec);
  CHECK(es.has_value() && es.value() == sizeof(buf));
  req_len_ = absl::little_endian::Load64(buf);

  CHECK_LE(req_len_, 1UL << 18);
  work_buf_.reset(new uint8_t[req_len_]);

  while (true) {
    ec = ReadMsg(&sz);
    if (FiberSocket::IsConnClosed(ec)) {
      LOG(INFO) << "Closing " << socket_->RemoteEndpoint();
      break;
    }
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
  connections.IncBy(-1);
}

class EchoListener : public ListenerInterface {
 public:
  virtual Connection* NewConnection(ProactorBase* context) final {
    return new EchoConnection;
  }
};

void RunServer(ProactorPool* pp) {
  ping_qps.Init(pp);
  connections.Init(pp);

  AcceptServer acceptor(pp);
  acceptor.set_back_log(FLAGS_backlog);

  acceptor.AddListener(FLAGS_port, new EchoListener);
  if (FLAGS_http_port >= 0) {
    uint16_t port = acceptor.AddListener(FLAGS_http_port, new HttpListener<>);
    LOG(INFO) << "Started http server on port " << port;
  }

  acceptor.Run();
  acceptor.Wait();
}

class Driver {
  std::unique_ptr<FiberSocketBase> socket_;

  Driver(const Driver&) = delete;

 public:
  Driver(ProactorBase* p);

  void Connect(unsigned index, const tcp::endpoint& ep);
  void Run(base::Histogram* dest);

 private:
  uint8_t buf_[8];
};

Driver::Driver(ProactorBase* p) {
  socket_.reset(p->CreateSocket());
}

void Driver::Connect(unsigned index, const tcp::endpoint& ep) {
  size_t iter = 0;
  size_t kMaxIter = 3;
  VLOG(1) << "Driver::Connect-Start " << index;
  for (; iter < kMaxIter; ++iter) {
    uint64_t start = absl::GetCurrentTimeNanos();
    auto ec = socket_->Connect(ep);
    CHECK(!ec) << ec.message();
    VLOG(1) << "Connected to " << socket_->RemoteEndpoint();

    uint64_t start1 = absl::GetCurrentTimeNanos();
    uint64_t delta_msec = (start1 - start) / 1000000;
    LOG_IF(ERROR, delta_msec > 1000) << "Slow connect1 " << index << " " << delta_msec << " ms";

    // Send msg size.
    absl::little_endian::Store64(buf_, FLAGS_size);
    uring::FiberSocket::expected_size_t es = socket_->Send(asio::buffer(buf_));
    CHECK(es) << es.error();  // Send expected payload size.
    CHECK_EQ(8U, es.value());

    uint64_t start2 = absl::GetCurrentTimeNanos();
    delta_msec = (start2 - start) / 1000000;
    LOG_IF(ERROR, delta_msec > 2000) << "Slow connect2 " << index << " " << delta_msec << " ms";

    es = socket_->Recv(asio::buffer(buf_, 1));
    delta_msec = (absl::GetCurrentTimeNanos() - start2) / 1000000;
    LOG_IF(ERROR, delta_msec > 2000) << "Slow connect3 " << index << " " << delta_msec << " ms";

    if (es) {
      CHECK_EQ(1U, es.value());
      break;
    }

    // There could be scenario where tcp Connect succeeds, but socket in fact is not really
    // connected (which I suspect happens due to small accept queue) and
    // we discover this upon first Recv. I am not sure why it happens. Right now I retry.
    CHECK(es.error() == std::errc::connection_reset) << es.error();
    socket_->Close();
    LOG(WARNING) << "Driver " << index << " retries";
  }
  CHECK_LT(iter, kMaxIter) << "Maximum reconnects reached";
  VLOG(1) << "Driver::Connect-End " << index;
}

void Driver::Run(base::Histogram* dest) {
  base::Histogram hist;

  std::unique_ptr<uint8_t[]> msg(new uint8_t[FLAGS_size]);


  iovec vec[2];
  vec[0].iov_len = 4;
  vec[0].iov_base = buf_;
  vec[1].iov_len = FLAGS_size;
  vec[1].iov_base = msg.get();

  auto lep = socket_->LocalEndpoint();
  CHECK(socket_->IsOpen());
  for (unsigned i = 0; i < FLAGS_n; ++i) {
    auto start = absl::GetCurrentTimeNanos();
    FiberSocketBase::expected_size_t es = socket_->Send(asio::buffer(msg.get(), FLAGS_size));
    CHECK(es.has_value()) << es.error();
    CHECK_EQ(es.value(), FLAGS_size);

    // DVLOG(1) << "Recv " << lep << " " << i;
    es = socket_->Recv(vec, 1);
    CHECK(es.has_value()) << "RecvError: " << es.error() << "/" << lep;
    auto res2 = socket_->Recv(vec + 1, 1);
    CHECK(res2.has_value()) << "Sock " << socket_->LocalEndpoint() << " " << i
                            << res2.error().message();
    CHECK_EQ(res2.value(), size_t(FLAGS_size));

    uint64_t dur = absl::GetCurrentTimeNanos() - start;
    hist.Add(dur / 1000);
  }

  socket_->Shutdown(SHUT_RDWR);
  dest->Merge(hist);
}

mutex lat_mu;
base::Histogram lat_hist;

class TLocalClient {
  ProactorBase* p_;
  vector<std::unique_ptr<Driver>> drivers_;

  TLocalClient(const TLocalClient&) = delete;
 public:
  TLocalClient(ProactorBase* p) : p_(p) {
    drivers_.resize(FLAGS_c);
    for (size_t i = 0; i < drivers_.size(); ++i) {
      drivers_[i].reset(new Driver{p});
    }
  }

  void Connect(tcp::endpoint ep);
  void Run();
};

void TLocalClient::Connect(tcp::endpoint ep) {
  LOG(INFO) << "TLocalClient::Connect-Start";
  vector<fibers::fiber> fbs(drivers_.size());
  for (size_t i = 0; i < fbs.size(); ++i) {
    fbs[i] = fibers::fiber([&, i] {
      this_fiber::properties<FiberProps>().set_name(absl::StrCat("connect/", i));
      uint64_t start = absl::GetCurrentTimeNanos();
      drivers_[i]->Connect(i, ep);
      uint64_t delta_msec = (absl::GetCurrentTimeNanos() - start) / 1000000;
      LOG_IF(ERROR, delta_msec > 4000) << "Slow DriverConnect " << delta_msec << " ms";
    });
  }
  for (auto& fb : fbs)
    fb.join();
  LOG(INFO) << "TLocalClient::Connect-End";
  google::FlushLogFiles(google::INFO);
}

void TLocalClient::Run() {
  this_fiber::properties<FiberProps>().set_name("RunClient");
  base::Histogram hist;

  LOG(INFO) << "RunClient " << p_->GetIndex();

  vector<fibers::fiber> fbs(drivers_.size());
  for (size_t i = 0; i < fbs.size(); ++i) {
    fbs[i] = fibers::fiber([&, i] {
      this_fiber::properties<FiberProps>().set_name(absl::StrCat("run/", i));
      drivers_[i]->Run(&hist);
    });
  }

  for (auto& fb : fbs)
    fb.join();
  unique_lock<mutex> lk(lat_mu);
  lat_hist.Merge(hist);
}


int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  CHECK_GT(FLAGS_port, 0);

  std::unique_ptr<ProactorPool> pp;
  if (FLAGS_epoll) {
    pp.reset(new epoll::EvPool);
  } else {
    pp.reset(new UringPool);
  }
  pp->Run();

  if (FLAGS_connect.empty()) {
    RunServer(pp.get());
  } else {
    CHECK_GT(FLAGS_size, 0U);

    // the simplest way to support dns resolution is to use asio resolver.
    asio::io_context io_context;
    tcp::resolver resolver{io_context};
    system::error_code ec;
    auto const results = resolver.resolve(FLAGS_connect, absl::StrCat(FLAGS_port), ec);
    CHECK(!ec) << "Could not resolve " << FLAGS_connect << " " << ec.message();
    CHECK(!results.empty());
    thread_local std::unique_ptr<TLocalClient> client;
    pp->AwaitFiberOnAll([&](auto* p) {
      client.reset(new TLocalClient(p));
      client->Connect(*results.begin());
    });

    auto start = absl::GetCurrentTimeNanos();
    pp->AwaitFiberOnAll([&](auto* p) {
      client->Run();
    });
    auto dur = absl::GetCurrentTimeNanos() - start;
    size_t dur_ms = std::max<size_t>(1, dur / 1000000);
    size_t dur_sec = std::max<size_t>(1, dur_ms / 1000);

    CONSOLE_INFO << "Total time " << dur_ms
                 << " ms, average qps: " << (pp->size() * size_t(FLAGS_c) * FLAGS_n) / dur_sec
                 << "\n";
    CONSOLE_INFO << "Overall latency (usec) \n" << lat_hist.ToString();
  }
  pp->Stop();

  return 0;
}
