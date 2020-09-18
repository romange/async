#include "file/file.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>

#include "base/logging.h"

using namespace std;

namespace file {
using nonstd::expected;
using nonstd::make_unexpected;

namespace {

static ssize_t read_all(int fd, uint8* buffer, size_t length, size_t offset) {
  size_t left_to_read = length;
  uint8* curr_buf = buffer;
  while (left_to_read > 0) {
    ssize_t read = pread(fd, curr_buf, left_to_read, offset);
    if (read <= 0) {
      return read == 0 ? length - left_to_read : read;
    }

    curr_buf += read;
    offset += read;
    left_to_read -= read;
  }
  return length;
}

// Returns true if a uint64 actually looks like a negative int64. This checks
// if the most significant bit is one.
//
// This function exists because the file interface declares some length/size
// fields to be uint64, and we want to catch the error case where someone
// accidently passes an negative number to one of the interface routines.
inline bool IsUInt64ANegativeInt64(uint64 num) {
  return (static_cast<int64>(num) < 0);
}
}  // namespace


bool Exists(absl::string_view fname) {
  return access(fname.data(), F_OK) == 0;
}

bool Delete(absl::string_view name) {
  int err;
  if ((err = unlink(name.data())) == 0) {
    return true;
  } else {
    return false;
  }
}

ReadonlyFile::~ReadonlyFile() {
}

// pread() based access.
class PosixReadFile final : public ReadonlyFile {
 private:
  int fd_;
  const size_t file_size_;
  bool drop_cache_;

 public:
  PosixReadFile(int fd, size_t sz, int advice, bool drop)
      : fd_(fd), file_size_(sz), drop_cache_(drop) {
    posix_fadvise(fd_, 0, 0, advice);
  }

  virtual ~PosixReadFile() {
    Close();
  }

  error_code Close() override {
    if (fd_) {
      if (drop_cache_)
        posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
      close(fd_);
      fd_ = 0;
    }
    return error_code{};
  }

  ExpectedSize Read(size_t offset, const Bytes& range) override {
    if (range.empty())
      return 0;

    if (offset > file_size_) {
      return make_unexpected(make_error_code(errc::argument_out_of_domain));
    }
    ssize_t r = read_all(fd_, range.begin(), range.size(), offset);
    if (r < 0) {
      return make_unexpected(StatusFileError());
    }

    return r;
  }

  size_t Size() const final {
    return file_size_;
  }

  int Handle() const final {
    return fd_;
  };
};

expected<ReadonlyFile*, ::std::error_code> OpenLocal(absl::string_view name,
                                                     const ReadonlyFile::Options& opts) {
  int fd = open(name.data(), O_RDONLY);
  if (fd < 0) {
    return make_unexpected(StatusFileError());
  }
  struct stat sb;
  if (fstat(fd, &sb) < 0) {
    close(fd);
    return make_unexpected(StatusFileError());
  }

  int advice = opts.sequential ? POSIX_FADV_SEQUENTIAL : POSIX_FADV_NORMAL;
  return new PosixReadFile(fd, sb.st_size, advice, opts.drop_cache_on_close);
}

}  // namespace file

namespace std {}  // namespace std
