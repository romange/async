// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include <absl/strings/ascii.h>

#include "base/init.h"
#include "base/io_buf.h"
#include "examples/pingserver/resp_parser.h"
#include "util/accept_server.h"
#include "util/asio_stream_adapter.h"
#include "util/http_handler.h"
#include "util/tls/tls_socket.h"
#include "util/uring/fiber_socket.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/uring/uring_pool.h"
#include "util/varz.h"

using namespace boost;
using namespace util;
using uring::FiberSocket;
using uring::Proactor;
using uring::SubmitEntry;
using uring::UringPool;
using redis::RespParser;
using IoResult = Proactor::IoResult;

DEFINE_int32(http_port, 8080, "Http port.");
DEFINE_int32(port, 6380, "Redis port");
DEFINE_bool(tls, false, "Enable tls");
DEFINE_bool(tls_verify_peer, false,
            "Require peer certificate. Please note that this flag requires loading of "
            "server certificates (not sure why).");

DEFINE_string(tls_cert, "", "");
DEFINE_string(tls_key, "", "");

VarzQps ping_qps("ping-qps");


inline void ToUpper(const RespParser::Buffer* val) {
  for (auto& c : *val) {
    c = absl::ascii_toupper(c);
  }
}

static inline absl::string_view ToAbsl(const absl::Span<uint8_t>& s) {
  return absl::string_view{reinterpret_cast<char*>(s.data()), s.size()};
}


class PingConnection : public Connection {
 public:
  PingConnection(SSL_CTX* ctx) : ctx_(ctx) {
  }

  void Handle(IoResult res, int32_t payload, Proactor* mgr);

  void StartPolling(int fd, Proactor* mgr);

 private:
  void HandleRequests() final;

  SSL_CTX* ctx_ = nullptr;
};

void PingConnection::HandleRequests() {
  system::error_code ec;
  std::unique_ptr<tls::TlsSocket> tls_sock;
  if (ctx_) {
    tls_sock.reset(new tls::TlsSocket(socket_.get()));
    tls_sock->InitSSL(ctx_);

    FiberSocketBase::accept_result aresult = tls_sock->Accept();
    if (!aresult) {
      LOG(ERROR) << "Error handshaking " << aresult.error().message();
      return;
    } else {
      LOG(INFO) << "TLS handshake succeeded";
    }
  }
  FiberSocketBase* peer = tls_sock ? (FiberSocketBase*)tls_sock.get() : socket_.get();

  AsioStreamAdapter<FiberSocketBase> asa(*peer);
  base::IoBuf io_buf{1024};
  RespParser resp_parser;
  uint32_t consumed = 0;
  std::vector<RespParser::Buffer> args;
  while (true) {
    auto dest = io_buf.AppendBuffer();
    asio::mutable_buffer mb(dest.data(), dest.size());
    size_t res = asa.read_some(mb, ec);
    if (FiberSocket::IsConnClosed(ec))
      break;

    CHECK(!ec) << ec << "/" << ec.message();
    VLOG(1) << "Read " << res << " bytes";
    io_buf.CommitWrite(res);
    RespParser::Status st = resp_parser.Parse(io_buf.InputBuffer(), &consumed, &args);
    io_buf.ConsumeInput(consumed);

    if (st == RespParser::RESP_OK) {
      if (args.empty())
        continue;
      ToUpper(&args[0]);
      absl::string_view cmd = ToAbsl(args.front());
      if (cmd == "PING") {
        ping_qps.Inc();
        const char kReply[] = "+PONG\r\n";
        auto b = boost::asio::buffer(kReply, sizeof(kReply) - 1);
        asa.write_some(b, ec);
        if (ec) {
          break;
        }
      } else {
        const char kReply[] = "+OK\r\n";
        auto b = boost::asio::buffer(kReply, sizeof(kReply) - 1);
        asa.write_some(b, ec);
        if (ec) {
          break;
        }
      }
    } else if (st == RespParser::MORE_INPUT) {
      continue;
    } else {
      break;
    }
  }

  VLOG(1) << "Connection shutting down";
  socket_->Shutdown(SHUT_RDWR);
}

class PingListener : public ListenerInterface {
 public:
  PingListener(SSL_CTX* ctx) : ctx_(ctx) {
  }

  ~PingListener() {
    SSL_CTX_free(ctx_);
  }

  virtual Connection* NewConnection(ProactorBase* context) final {
    return new PingConnection(ctx_);
  }

 private:
  SSL_CTX* ctx_;
};

static int MyVerifyCb(int preverify_ok, X509_STORE_CTX* x509_ctx) {
  LOG(INFO) << "preverify " << preverify_ok;
  return 1;
}

// To connect: openssl s_client  -cipher "ADH:@SECLEVEL=0" -state -crlf  -connect 127.0.0.1:6380
static SSL_CTX* CreateSslCntx() {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());

  // TO connect with redis-cli:
  // ./src/redis-cli --tls -p 6380 --insecure  PING
  // For redis-cli we need to load certificate in order to use a common cipher.
  if (!FLAGS_tls_key.empty()) {
    CHECK_EQ(1, SSL_CTX_use_PrivateKey_file(ctx, FLAGS_tls_key.c_str(), SSL_FILETYPE_PEM));

    if (!FLAGS_tls_cert.empty()) {
      CHECK_EQ(1, SSL_CTX_use_certificate_chain_file(ctx, FLAGS_tls_cert.c_str()));
    }
  }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  SSL_CTX_set_options(ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);

  unsigned mask = SSL_VERIFY_PEER;
  if (FLAGS_tls_verify_peer)
    mask |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  SSL_CTX_set_verify(ctx, mask, MyVerifyCb);
  // SSL_CTX_set_verify_depth(ctx, 0);

  // Can also be defined using "ADH:@SECLEVEL=0" cipher string below.
  SSL_CTX_set_security_level(ctx, 0);

  CHECK_EQ(1, SSL_CTX_set_cipher_list(ctx, "ADH:DEFAULT"));
  CHECK_EQ(1, SSL_CTX_set_dh_auto(ctx, 1));

  return ctx;
}

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  CHECK_GT(FLAGS_port, 0);

  SSL_CTX* ctx = nullptr;
  if (FLAGS_tls) {
    ctx = CreateSslCntx();
  }

  UringPool pp;
  pp.Run();
  ping_qps.Init(&pp);

  AcceptServer uring_acceptor(&pp);
  uring_acceptor.AddListener(FLAGS_port, new PingListener(ctx));
  if (FLAGS_http_port >= 0) {
    uint16_t port = uring_acceptor.AddListener(FLAGS_http_port, new HttpListener<>);
    LOG(INFO) << "Started http server on port " << port;
  }

  uring_acceptor.Run();
  uring_acceptor.Wait();

  pp.Stop();

  return 0;
}
