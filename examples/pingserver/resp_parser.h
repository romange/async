// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include <array>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace redis {

class RespParser {
  static constexpr uint16_t kBufSz = 256;

 public:
  using Buffer = absl::Span<uint8_t>;
  enum ParseStatus { LINE_FINISHED = 1, MORE_DATA = 2 };

  RespParser();

  //! 0-copy interface
  Buffer GetDestBuf() const {
    return Buffer(write_start_, buf_.data() + kBufSz - write_start_);
  }

  void WriteCommit(size_t write_sz) { write_start_ += write_sz; }

  //! Tries to parse the next line in the buffer ending with \r\n.
  //! If succeeds, sets the 'line' argument to point to the buffer representing the line and
  //! returns LINE_FINISHED. Otherwise, returns MORE_DATA.
  //! In case of LINE_FINISHED, the returned buffer is "consumed" but still valid until the next
  // operation to RespParser.
  ParseStatus ParseNext(absl::string_view* line);

  Buffer ReadBuf() const {
    return Buffer(next_read_, write_start_ - next_read_);
  }

  bool IsReadEof() const { return next_read_ == write_start_; }

  void Reset() {
    write_start_ = buf_.data();
    next_parse_ = next_read_ = write_start_;
  }

  // Consumes and discards 'sz' characters.
  void Consume(size_t sz) {
    next_read_ += sz;
    Realign();
  }

  void ConsumeLine() {
    Realign();
  }

  void Realign();

  void ConsumeWs();
 private:
  uint8_t* WriteEnd() {
    return buf_.data() + kBufSz;
  }

  std::array<uint8_t, kBufSz + 4> buf_;

  // The structure is: |buf_.data()____next_read_____next_parse____write_start____buf_.end()|
  uint8_t *write_start_, *next_read_ = nullptr, *next_parse_ = nullptr;
};

}  // namespace redis
