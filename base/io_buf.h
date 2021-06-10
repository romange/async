// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#pragma once

#include <absl/types/span.h>

#include <cstring>
#include <memory>

namespace base {

class IoBuf {
 public:
  using Bytes = absl::Span<uint8_t>;

  explicit IoBuf(size_t capacity) : buf_(new uint8_t[capacity]), capacity_(capacity) {
    next_ = end_ = buf_.get();
  }

  uint8_t* Input() {
    return next_;
  }
  size_t Capacity() const {
    return capacity_;
  }

  size_t InputBytes() const {
    return end_ - next_;
  }
  bool Empty() const {
    return end_ == next_;
  }

  void ConsumeInput(size_t offs);

  void CopyAndConsume(size_t sz, void* dest) {
    memcpy(dest, Input(), sz);
    ConsumeInput(sz);
  }

  Bytes AppendBuf() {
    return Bytes(end_, (buf_.get() + capacity_) - end_);
  }

  void CommitWrite(size_t sz) {
    end_ += sz;
  }

  // Deprecated.
  void AppendSize(size_t sz) {
    CommitWrite(sz);
  }

 private:
  size_t ReadPos() const {
    return next_ - buf_.get();
  }

  std::unique_ptr<uint8_t[]> buf_;
  size_t capacity_ = 0;
  uint8_t* next_;
  uint8_t* end_;
};

inline void IoBuf::ConsumeInput(size_t offs) {
  next_ += offs;
  if (Empty()) {
    next_ = end_ = buf_.get();
    return;
  }

  size_t av = InputBytes();
  if (av <= 16U && ReadPos() > 16U) {
    memcpy(buf_.get(), next_, av);
    next_ = buf_.get();
    end_ = next_ + av;
  }
}

}  // namespace base
