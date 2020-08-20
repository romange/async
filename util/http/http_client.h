// Copyright 2018, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>

#include "strings/stringpiece.h"

namespace util {
class IoContext;
class FiberSyncSocket;

namespace http {

/*
  Single threaded, fiber-friendly synchronous client: Upon IO block, the calling fiber blocks
  but the thread can switch to other active fibers.
*/
class Client {
 public:
  using Response = boost::beast::http::response<boost::beast::http::dynamic_body>;
  using Verb = boost::beast::http::verb;

  explicit Client(IoContext* io_context);
  ~Client();

  boost::system::error_code Connect(StringPiece host, StringPiece service);

  boost::system::error_code Send(Verb verb, StringPiece url, StringPiece body, Response* response);
  boost::system::error_code Send(Verb verb, StringPiece url, Response* response) {
    return Send(verb, url, StringPiece{}, response);
  }

  void Shutdown();

  bool IsConnected() const;

  void set_connect_timeout_ms(uint32_t ms) { connect_timeout_ms_ = ms; }

  // Adds header to all future requests.
  void AddHeader(std::string name, std::string value) {
    headers_.emplace_back(std::move(name), std::move(value));
  }

 private:
  IoContext& io_context_;
  uint32_t connect_timeout_ms_ = 2000;

  using HeaderPair = std::pair<std::string, std::string>;

  std::vector<HeaderPair> headers_;
  std::unique_ptr<FiberSyncSocket> socket_;
};

}  // namespace http
}  // namespace util

