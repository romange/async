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

class LocalWriteFile : public WriteFile {
 public:
  // flags defined at http://man7.org/linux/man-pages/man2/open.2.html
  LocalWriteFile(absl::string_view file_name, int flags) : WriteFile(file_name), flags_(flags) {
  }

  virtual ~LocalWriteFile() override;

  error_code Close() override;

  error_code Write(const uint8* buffer, uint64 length) final;

  error_code Open();

 protected:
  int fd_ = -1;
  int flags_;
};

LocalWriteFile::~LocalWriteFile() {
}

error_code LocalWriteFile::Open() {
  CHECK_EQ(fd_, -1);

  fd_ = open(create_file_name_.c_str(), flags_, 0644);
  if (fd_ < 0) {
    return StatusFileError();
  }
  return error_code{};
}

error_code LocalWriteFile::Close() {
  int res = 0;
  if (fd_ > 0) {
    res = close(fd_);
    fd_ = -1;
  }
  return res < 0 ? StatusFileError() : error_code{};
}

error_code LocalWriteFile::Write(const uint8* buffer, uint64 length) {
  DCHECK(buffer);
  DCHECK(!IsUInt64ANegativeInt64(length));

  uint64 left_to_write = length;
  while (left_to_write > 0) {
    ssize_t written = write(fd_, buffer, left_to_write);
    if (written < 0) {
      return StatusFileError();
    }
    buffer += written;
    left_to_write -= written;
  }

  return error_code{};
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

  SizeOrError Read(size_t offset, const MutableBytes& range) override {
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

expected<ReadonlyFile*, ::error_code> OpenRead(absl::string_view name,
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

expected<WriteFile*, error_code> OpenWrite(absl::string_view file_name,
                                                   WriteFile::Options opts) {
  int flags = O_CREAT | O_WRONLY | O_CLOEXEC;
  if (opts.append)
    flags |= O_APPEND;
  else
    flags |= O_TRUNC;
  LocalWriteFile* ptr = new LocalWriteFile(file_name, flags);
  error_code ec = ptr->Open();
  if (ec) {
    delete ptr;
    return make_unexpected(ec);
  }
  return ptr;
}

WriteFile::WriteFile(absl::string_view name) : create_file_name_(name) {
}

WriteFile::~WriteFile() {
}

}  // namespace file

namespace std {}  // namespace std
