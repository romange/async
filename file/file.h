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

using SizeOrError = nonstd::expected<size_t, ::std::error_code>;

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

  using MutableBytes = absl::Span<uint8_t>;

  virtual ~ReadonlyFile();

  // Reads upto length bytes and updates the result to point to the data.
  // May use buffer for storing data. In case, EOF reached sets result.size() < length but still
  // returns Status::OK.
  ABSL_MUST_USE_RESULT virtual SizeOrError Read(size_t offset, const MutableBytes& range) = 0;

  // releases the system handle for this file. Does not delete this.
  ABSL_MUST_USE_RESULT virtual ::std::error_code Close() = 0;

  virtual size_t Size() const = 0;

  virtual int Handle() const = 0;
};

/*! @brief Abstract class for write, append only file (sink).
 *
 * Used for abstracting different append-only implementations including
 * local posix files, network based files etc. Each implementation may have different threading
 * guarantees. The default file::Open implementation returns non thread safe posix file.
 */
class WriteFile {
 public:
  struct Options {
    bool append = false;
  };

  virtual ~WriteFile();

  /*! @brief Flushes remaining data, closes access to a file handle. For asynchronous interfaces
      serves as a barrier and makes sure previous writes are flushed.
   *
   */
  virtual std::error_code Close() = 0;

  ABSL_MUST_USE_RESULT virtual std::error_code Write(const uint8* buffer, uint64 length) = 0;

  ABSL_MUST_USE_RESULT std::error_code Write(const absl::string_view slice) {
    return Write(reinterpret_cast<const uint8*>(slice.data()), slice.size());
  }

  //! Returns the file name given during Create(...) call.
  const std::string& create_file_name() const {
    return create_file_name_;
  }

  // May be used for asynchronous implementations in order to communicate the intermediate status
  // so far.
  virtual std::error_code Status() { return std::error_code{};}

  // By default not implemented but can be for asynchronous implementations. Does not return
  // status. Refer to Status() and Close() for querying the intermediate status.
  virtual void AsyncWrite(std::string blob) {}

 protected:
  explicit WriteFile(const absl::string_view create_file_name);

  // Name of the created file.
  const std::string create_file_name_;
};

//! Deletes the file returning true iff successful.
bool Delete(absl::string_view name);

bool Exists(absl::string_view name);

using ReadonlyFileOrError = nonstd::expected<ReadonlyFile*, ::std::error_code>;
ABSL_MUST_USE_RESULT ReadonlyFileOrError OpenRead(
    absl::string_view name, const ReadonlyFile::Options& opts);

using WriteFileOrError = nonstd::expected<WriteFile*, ::std::error_code>;

//! Factory method to create a new writable file object. Calls Open on the
//! resulting object to open the file.
ABSL_MUST_USE_RESULT WriteFileOrError OpenWrite(
    absl::string_view path, WriteFile::Options opts = WriteFile::Options());

}  // namespace file

namespace std {}  // namespace std
