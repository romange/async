// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "examples/pingserver/resp_parser.h"

#include "absl/strings/ascii.h"
#include "base/logging.h"

namespace redis {

namespace {

uint8_t* ParseLine(uint8_t* buf, const uint8_t* end) {
  uint8_t* start = buf;

  while (start < end) {
    size_t sz = end - start;
    uint8_t* next = reinterpret_cast<uint8_t*>(memchr(start, '\r', sz));
    if (!next)
      break;

    if (next[1] == '\n') {
      next[0] = '\0';
      return next;
    }
    start = next + 1;
  }
  return nullptr;
}

}  // namespace

RespParser::RespParser() : write_start_(buf_.begin()) {
  buf_[kBufSz] = '\r';
  buf_[kBufSz + 1] = '\n';
  next_read_ = write_start_;
  next_parse_ = write_start_;
}

auto RespParser::ParseNext(absl::string_view* line) -> ParseStatus {
  if (write_start_ - next_parse_ < 2)
    return ParseStatus::MORE_DATA;

  uint8_t* tmp = ParseLine(next_parse_, write_start_);
  if (tmp) {
    *line = absl::string_view(reinterpret_cast<char*>(next_read_), tmp - next_read_);
    next_read_ = tmp + 2;
    next_parse_ = next_read_;
    DVLOG(1) << "NR: " << next_read_ - buf_.begin() << ", ws: " << write_start_ - buf_.begin();
    // We can not realign here because we will ruin the *line.

    return LINE_FINISHED;
  }

  next_parse_ = write_start_ - 1;
  DVLOG(1) << "NR: " << next_read_ - buf_.begin() << ", ws: " << write_start_ - buf_.begin();

  return MORE_DATA;
}

// Makes enough space for write opertion by possibly moving the unread block the left.
void RespParser::Realign() {
  if (write_start_ == buf_.data())
    return;

  ssize_t kept_len = write_start_ - next_read_;
  CHECK_GE(kept_len, 0);

  if (kept_len == 0) {
    write_start_ = buf_.data();
    next_read_ = write_start_;
    next_parse_ = write_start_;
  } else if (kept_len < 64) {
    memmove(buf_.data(), next_read_, kept_len);
    next_read_ = buf_.data();
    write_start_ = buf_.data() + kept_len;
    next_parse_ = next_read_;
  }
}

void RespParser::ConsumeWs() {
  for (; next_read_ < write_start_; ++next_read_) {
    if (!absl::ascii_isspace(*next_read_))
      break;
  }
  Realign();
  next_parse_  = next_read_;
}

}  // namespace redis
