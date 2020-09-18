// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <system_error>
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

#include "base/expected.hpp"
#include "base/integral_types.h"

namespace file {

inline ::std::error_code StatusFileError() {
  return ::std::error_code(errno, ::std::generic_category());
}

// ReadonlyFile objects are created via ReadonlyFile::Open() factory function
// and are destroyed via "obj->Close(); delete obj;" sequence.
//
class ReadonlyFile {
 protected:
  ReadonlyFile() {
  }

 public:
  struct Options {
    bool sequential = true;
    bool drop_cache_on_close = true;
    Options() {
    }
  };

  using ExpectedSize = nonstd::expected<size_t, ::std::error_code>;
  using Bytes = absl::Span<uint8_t>;

  virtual ~ReadonlyFile();

  // Reads upto length bytes and updates the result to point to the data.
  // May use buffer for storing data. In case, EOF reached sets result.size() < length but still
  // returns Status::OK.
  ABSL_MUST_USE_RESULT virtual ExpectedSize Read(size_t offset, const Bytes& range) = 0;

  // releases the system handle for this file. Does not delete this.
  ABSL_MUST_USE_RESULT virtual ::std::error_code Close() = 0;

  virtual size_t Size() const = 0;

  virtual int Handle() const = 0;
};

//! Deletes the file returning true iff successful.
bool Delete(absl::string_view name);

bool Exists(absl::string_view name);

ABSL_MUST_USE_RESULT nonstd::expected<ReadonlyFile*, ::std::error_code>
  OpenLocal(absl::string_view name, const ReadonlyFile::Options& opts);

}  // namespace file

namespace std {


}  // namespace std
