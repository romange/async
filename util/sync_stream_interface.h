// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <sys/uio.h>
#include "base/expected.hpp"

namespace util {

class SyncStreamInterface {
public:
  using expected_size_t = nonstd::expected<size_t, std::error_code>;

  virtual ~SyncStreamInterface() {}

  virtual expected_size_t Send(const iovec* ptr, size_t len) = 0;
  virtual expected_size_t Recv(iovec* ptr, size_t len) = 0;
};

}  // namespace util
