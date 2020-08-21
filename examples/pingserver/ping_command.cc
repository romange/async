// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "examples/pingserver/ping_command.h"
#include "base/logging.h"
#include "absl/strings/escaping.h"

const char PingCommand::kReply[] = "+PONG\r\n";
using namespace boost;

bool PingCommand::Decode(size_t len) {
  resp_parser_.WriteCommit(len);

  absl::string_view line;
  redis::RespParser::ParseStatus status = resp_parser_.ParseNext(&line);

  bool res = false;

  while (status == redis::RespParser::LINE_FINISHED) {
    VLOG(1) << "Line " << line;

    res = HandleLine(line);
    if (res)
      break;

    status = resp_parser_.ParseNext(&line);
  }
  resp_parser_.Realign();

  return res;
}

bool PingCommand::HandleLine(absl::string_view line) {
  switch (state_) {
    case READ_NUM_TOKS:
      // benchmark-cli can send the "SIMPLE" command.
      if (line == "PING") {
        return true;
      }

      CHECK_EQ("*1", line) << absl::CEscape(line);
      state_ = STR_LEN;
      break;
    case STR_LEN:
      CHECK_EQ(line, "$4");
      state_ = STR_DATA;
      break;
    case STR_DATA:
      CHECK_EQ(line, "PING");
      state_ = READ_NUM_TOKS;
      return true;
      break;
  }
  return false;
}

boost::asio::const_buffer PingCommand::reply() const {
  return boost::asio::buffer(kReply, sizeof(kReply) - 1);
}

using asio::ip::tcp;

void ConfigureSocket(boost::asio::ip::tcp::socket* sock) {
  tcp::no_delay nd(true);
  sock->set_option(nd);

  tcp::socket::keep_alive opt2(true);
  sock->set_option(opt2);
  sock->get_option(opt2);

  sock->get_option(opt2);
  CHECK(opt2.value());
  sock->get_option(nd);
  CHECK(nd.value());

  int val = 300;
  CHECK_EQ(0, setsockopt(sock->native_handle(), IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)));

  val = 100;
  CHECK_EQ(0, setsockopt(sock->native_handle(), IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)));

  val = 3;
  CHECK_EQ(0, setsockopt(sock->native_handle(), IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)));
}
