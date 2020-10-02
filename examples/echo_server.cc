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
DEFINE_uint32(size, 0, "Message size, 0 for hardcoded 4 byte pings");
DEFINE_string(connect, "", "hostname or ip address to connect to in client mode");

uring::VarzQps ping_qps("ping-qps");

class EchoConnection : public uring::Connection {
 public:
  EchoConnection() {
    work_buf_.reset(new uint8_t[1 << 16]);
  }

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
  uint8_t buf[8];

  if (FLAGS_size <= 0) {
#if 0
    Proactor* p = socket_.proactor();

    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = vec;
    msg.msg_iovlen = 1;
#endif
    vec[0].iov_base = buf;
    vec[0].iov_len = 8;
    uint64 num_req = 0;
    while (true) {
      auto res = socket_.Recv(asio::buffer(buf, 8));
      if (!res.has_value()) {
        if (!FiberSocket::IsConnClosed(res.error())) {
          LOG(WARNING) << "Broke on " << res.error();
        }
        break;
      }
      CHECK_EQ(8u, res.value());

      int64_t sender_ts = absl::little_endian::Load64(buf);
      int64_t recv_now = absl::GetCurrentTimeNanos();

      absl::little_endian::Store64(buf, recv_now);
      res = socket_.Send(asio::buffer(buf, 8));
      if (res.has_value()) {
        CHECK_EQ(8u, res.value());
      } else {
        if (!FiberSocket::IsConnClosed(res.error())) {
          LOG(INFO) << "Broke on " << res.error();
        }
        break;
      }

      if (++num_req > 10) {
        int64_t recv_delay_usec = (recv_now - sender_ts) / 1000;
        int64_t send_del_usec = (absl::GetCurrentTimeNanos() - recv_now) / 1000;
        if (recv_delay_usec >= 10000 || send_del_usec > 5000) {
          LOG(INFO) << "Recv delay: " << recv_delay_usec << ", send delay " << send_del_usec;
        }
      }
    }
  } else {
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
      auto res = socket_.Send(vec, 2);
      CHECK(res.has_value());
    }
  }
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
  FiberSocket socket_;

  Driver(const Driver&) = delete;

 public:
  Driver(const tcp::endpoint& ep, Proactor* p);

  void Run(base::Histogram* dest);

 private:
  void SendRcvPing(Proactor* p);
  msghdr msg_;
  uint8_t buf_[8];
  iovec vec_[2];
};

Driver::Driver(const tcp::endpoint& ep, Proactor* p) : socket_(p) {
  auto ec = socket_.Connect(ep);
  CHECK(!ec) << ec;
  VLOG(1) << "Connected to " << socket_.RemoteEndpoint();
  memset(&msg_, 0, sizeof(msg_));
  msg_.msg_iov = vec_;
  msg_.msg_iovlen = 1;
  vec_[0].iov_base = buf_;
  vec_[0].iov_len = 8;
}

void Driver::SendRcvPing(Proactor* p) {
  uring::SubmitEntry se1 = p->GetSubmitEntry(nullptr, 0);
  se1.PrepSendMsg(socket_.RealFd(), &msg_, 0);
  se1.sqe()->flags |= IOSQE_IO_LINK;
  auto res = socket_.Recv(asio::buffer(buf_, 8));
  CHECK(res.has_value());
  CHECK_EQ(8u, res.value());
}

void Driver::Run(base::Histogram* dest) {
  vec_[0].iov_base = buf_;
  vec_[0].iov_len = 8;
  base::Histogram hist;

  if (FLAGS_size <= 0) {
    Proactor* p = socket_.proactor();
    // Warmup
    for (unsigned i = 0; i < 10; ++i) {
      uint64_t start = absl::GetCurrentTimeNanos();
      absl::little_endian::Store64(buf_, start);
      SendRcvPing(p);
    }

    for (unsigned i = 0; i < FLAGS_n; ++i) {
      uint64_t start = absl::GetCurrentTimeNanos();
      absl::little_endian::Store64(buf_, start);

      SendRcvPing(p);
      uint64_t now = absl::GetCurrentTimeNanos();
      uint64_t dur_usec = (now - start) / 1000;
      uint64_t srv_snd = absl::little_endian::Load64(buf_);
      hist.Add(dur_usec);
      if (dur_usec > 20000) {  // 20ms
        LOG(INFO) << "RTT " << dur_usec << ", recv delay " << (now - srv_snd) / 1000;
      }
    }
  } else {
    absl::little_endian::Store32(buf_, FLAGS_size);
    std::unique_ptr<uint8_t[]> msg(new uint8_t[FLAGS_size]);

    vec_[0].iov_len = 4;
    vec_[1].iov_base = msg.get();
    vec_[1].iov_len = FLAGS_size;

    for (unsigned i = 0; i < FLAGS_n; ++i) {
      auto start = absl::GetCurrentTimeNanos();
      auto res = socket_.Send(vec_, 2);
      CHECK(res.has_value()) << res.error();
      CHECK_EQ(res.value(), size_t(FLAGS_size + 4));

      auto res2 = socket_.Recv(vec_, 2);
      CHECK(res.has_value()) << res.error();
      CHECK_EQ(res2.value(), size_t(FLAGS_size + 4));

      uint64_t dur = absl::GetCurrentTimeNanos() - start;
      hist.Add(dur / 1000);
    }
  }
  socket_.Shutdown(SHUT_RDWR);
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
    size_t dur_ms = std::max<size_t>(1, dur / 1000000);
    size_t dur_sec = std::max<size_t>(1, dur_ms / 1000);

    CONSOLE_INFO << "Total time " << dur_ms
                 << " ms, average qps: " << (pp.size() * size_t(FLAGS_c) * FLAGS_n) / dur_sec
                 << "\n";
    CONSOLE_INFO << "Overall latency (usec) " << lat_hist.ToString();
  }
  pp.Stop();

  return 0;
}