// Copyright 2018, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/http/http_client.h"

#include <boost/asio/connect.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include "base/logging.h"
#include "util/asio/fiber_socket.h"
#include "util/asio/yield.h"

namespace util {
namespace http {

using fibers_ext::yield;
using namespace boost;

namespace h2 = beast::http;

Client::Client(IoContext* io_context) : io_context_(*io_context) {}

Client::~Client() {}

system::error_code Client::Connect(StringPiece host, StringPiece service) {
  socket_.reset(
      new FiberSyncSocket(strings::AsString(host), strings::AsString(service), &io_context_));

  return socket_->ClientWaitToConnect(connect_timeout_ms_);
}

::boost::system::error_code Client::Send(Verb verb, StringPiece url, StringPiece body,
                                         Response* response) {
  // Set the URL
  h2::request<h2::string_body> req{verb, boost::string_view(url.data(), url.size()), 11};

  // Optional headers
  for (const auto& k_v : headers_) {
    req.set(k_v.first, k_v.second);
  }
  req.body().assign(body.begin(), body.end());
  req.prepare_payload();

  system::error_code ec;

  // Send the HTTP request to the remote host.
  h2::write(*socket_, req, ec);
  if (ec) {
    VLOG(1) << "Error " << ec;
    return ec;
  }

  // This buffer is used for reading and must be persisted
  beast::flat_buffer buffer;

  h2::read(*socket_, buffer, *response, ec);
  VLOG(2) << "Resp: " << *response;

  return ec;
}

void Client::Shutdown() {
  if (socket_) {
    system::error_code ec;
    socket_->Shutdown(ec);
    socket_.reset();
  }
}

bool Client::IsConnected() const {
  return socket_ && socket_->is_open() && !socket_->status();
}

}  // namespace http
}  // namespace util
