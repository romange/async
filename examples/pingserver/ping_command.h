// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "examples/pingserver/resp_parser.h"

class PingCommand {
  redis::RespParser resp_parser_;
  enum State { READ_NUM_TOKS, STR_LEN, STR_DATA } state_ = READ_NUM_TOKS;

  static const char kReply[];

 public:
  PingCommand() {
  }

  bool Decode(size_t len);

  boost::asio::mutable_buffer read_buffer() {
    auto span = resp_parser_.GetDestBuf();
    return boost::asio::buffer(span.data(), span.size());
  }

  boost::asio::const_buffer reply() const;

 private:
  bool HandleLine(absl::string_view line);
};

void ConfigureSocket(boost::asio::ip::tcp::socket* sock);
