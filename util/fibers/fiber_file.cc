// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/fibers/fiber_file.h"

#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <sys/uio.h>

#include <atomic>
#include <boost/fiber/mutex.hpp>

#include "base/hash.h"
#include "base/histogram.h"
#include "base/logging.h"

namespace util {
using namespace file;
using namespace std;
using nonstd::make_unexpected;
using namespace boost;

namespace {

ssize_t read_all(int fd, const iovec* iov, int iovcnt, size_t offset) {
  size_t left = std::accumulate(iov, iov + iovcnt, 0,
                                [](size_t a, const iovec& i2) { return a + i2.iov_len; });

  ssize_t completed = 0;
  iovec tmp_iov[iovcnt];

  std::copy(iov, iov + iovcnt, tmp_iov);
  iovec* next_iov = tmp_iov;
  while (true) {
    ssize_t read = preadv(fd, next_iov, iovcnt, offset);
    if (read <= 0) {
      return read == 0 ? completed : read;
    }

    left -= read;
    completed += read;
    if (left == 0)
      break;

    offset += read;
    while (next_iov->iov_len <= static_cast<size_t>(read)) {
      read -= next_iov->iov_len;
      ++next_iov;
      --iovcnt;
    }
    next_iov->iov_len -= read;
  }
  return completed;
}

class FiberReadFile : public ReadonlyFile {
 public:
  FiberReadFile(const FiberReadOptions& opts, ReadonlyFile* next,
                util::fibers_ext::FiberQueueThreadPool* tp);

  // Reads upto length bytes and updates the result to point to the data.
  // May use buffer for storing data. In case, EOF reached sets result.size() < length but still
  // returns Status::OK.
  SizeOrError Read(size_t offset, const MutableBytes& range) final;

  // releases the system handle for this file.
  ::std::error_code Close() final;

  size_t Size() const final {
    return next_->Size();
  }

  int Handle() const final {
    return next_->Handle();
  }

 private:
  SizeOrError ReadAndPrefetch(size_t offset, const MutableBytes& range);

  // Returns true if requires further prefetching.
  std::pair<size_t, bool> ReadFromCache(size_t offset, const MutableBytes& range);

  void HandleActivePrefetch();

  MutableBytes prefetch_;
  size_t file_prefetch_offset_ = -1;
  std::unique_ptr<uint8_t[]> buf_;
  size_t buf_size_ = 0;
  std::unique_ptr<ReadonlyFile> next_;

  fibers_ext::FiberQueueThreadPool* tp_;
  FiberReadOptions::Stats* stats_ = nullptr;
  fibers_ext::Done done_;
  base::Histogram tp_wait_hist_;

  std::atomic<ssize_t> prefetch_res_{0};
  uint8_t* prefetch_ptr_ = nullptr;
  absl::Time prefetch_start_ts_;
};

class WriteFileImpl : public WriteFile {
 public:
  WriteFileImpl(WriteFile* real, ssize_t hash, fibers_ext::FiberQueueThreadPool* tp)
      : WriteFile(real->create_file_name()), upstream_(real), tp_(tp), hash_(hash) {
  }

  error_code Close() final;

  error_code Write(const uint8* buffer, uint64 length) final;

  virtual ~WriteFileImpl() {
  }

  std::error_code Status() final {
    unique_lock<fibers::mutex> lk(mu_);
    return ec_;
  }

  // By default not implemented but can be for asynchronous implementations. Does not return
  // status. Refer to Status() and Close() for querying the intermediate status.
  void AsyncWrite(std::string blob) final;

 private:
  WriteFile* upstream_;

  fibers_ext::FiberQueueThreadPool* tp_;
  ssize_t hash_;

  fibers::mutex mu_;
  error_code ec_;
  atomic_bool has_error_{false};
};

/**** Implementation *********************/
FiberReadFile::FiberReadFile(const FiberReadOptions& opts, ReadonlyFile* next,
                             util::fibers_ext::FiberQueueThreadPool* tp)
    : next_(next), tp_(tp) {
  buf_size_ = opts.prefetch_size;
  if (buf_size_) {
    buf_.reset(new uint8_t[buf_size_]);
    prefetch_ = MutableBytes(buf_.get(), 0);
  }
  stats_ = opts.stats;
}

error_code FiberReadFile::Close() {
  if (prefetch_ptr_) {
    done_.Wait(AND_RESET);
    prefetch_ptr_ = nullptr;
  }
  VLOG(1) << "Read Histogram: " << tp_wait_hist_.ToString();

  return next_->Close();
}

auto FiberReadFile::ReadAndPrefetch(size_t offset, const MutableBytes& range) -> SizeOrError {
  size_t copied = 0;
  if (stats_)
    ++stats_->read_prefetch_cnt;

  if (prefetch_ptr_ || !prefetch_.empty()) {
    auto res = ReadFromCache(offset, range);
    if (!res.second)  // if we should not issue a prefetch request, we return what we read.
      return res.first;

    copied = res.first;
    offset += copied;
  }
  DCHECK(!prefetch_ptr_);  // no active pending requests at this point.

  // At this point prefetch_ must point at buf_ and might still contained prefetched slice.
  prefetch_ = MutableBytes(buf_.get(), prefetch_.size());

  iovec io[2] = {{range.data() + copied, range.size() - copied},
                 {buf_.get() + prefetch_.size(), buf_size_ - prefetch_.size()}};

  if (copied < range.size()) {  // We need to issue request to fill this read.
    absl::Time start = absl::Now();

    DCHECK(prefetch_.empty());

    ssize_t total_read = -1;

    // We issue 2 read requests: to fill user buffer and our prefetch buffer.
    tp_->Add([&] {
      total_read = read_all(next_->Handle(), io, 2, offset);
      done_.Notify();
    });
    done_.Wait(AND_RESET);
    if (VLOG_IS_ON(1)) {
      auto dur = absl::Now() - start;
      tp_wait_hist_.Add(absl::ToInt64Microseconds(dur));
    }

    if (stats_) {
      ++stats_->preempt_cnt;
      stats_->disk_bytes += io[0].iov_len;
    }
    if (total_read < 0)
      return make_unexpected(file::StatusFileError());
    if (static_cast<size_t>(total_read) <= io[0].iov_len)  // EOF
      return total_read + copied;

    file_prefetch_offset_ = offset + io[0].iov_len;
    total_read -= io[0].iov_len;  // reduce range part.

    prefetch_ = MutableBytes(buf_.get(), total_read);
    if (stats_) {
      stats_->cache_bytes += total_read;
    }
    return range.size();  // Fully read and possibly some prefetched.
  }

  // else: copied >= range.size() and we did not read from disk yet but we want to prefetch
  // data into non blocking storage.
  prefetch_ptr_ = reinterpret_cast<uint8_t*>(io[1].iov_base);
  struct Pending {
    iovec io;
    size_t offs;
  } pending{io[1], file_prefetch_offset_ + prefetch_.size()};

  // we filled range but we want to issue a readahead fetch.
  // We must keep reference to done_ in pending because of the shutdown flow.
  prefetch_start_ts_ = absl::Now();
  tp_->Add([this, pending = std::move(pending)]() mutable {
    prefetch_res_.store(read_all(next_->Handle(), &pending.io, 1, pending.offs),
                        std::memory_order_release);
    done_.Notify();
  });

  return range.size();
}

// Returns how much was read from cache and whether we should issue prefetch request following
// this read.
std::pair<size_t, bool> FiberReadFile::ReadFromCache(size_t offset, const MutableBytes& range) {
  bool should_prefetch =
      (range.size() > prefetch_.size() && prefetch_ptr_) || (offset != file_prefetch_offset_);
  if (should_prefetch) {
    HandleActivePrefetch();
  }

  std::pair<size_t, bool> res(0, true);
  if (offset != file_prefetch_offset_) {
    prefetch_ = MutableBytes{};
    return res;
  }

  DCHECK(prefetch_.end() <= buf_.get() + buf_size_);

  // We could put a smarter check but for sequential access it's enough.
  res.first = std::min(prefetch_.size(), range.size());

  memcpy(range.data(), prefetch_.data(), res.first);
  file_prefetch_offset_ += res.first;
  prefetch_.remove_prefix(res.first);

  if (prefetch_ptr_ || prefetch_.size() >= buf_size_ / 2) {
    // We do not need to issue prefetch request, either we've issued one already or we have
    // enough buffer to go forward.
    res.second = false;
  } else if (!prefetch_.empty()) {
    memmove(buf_.get(), prefetch_.data(), prefetch_.size());
  }

  return res;
}

void FiberReadFile::HandleActivePrefetch() {
  bool preempt = done_.Wait(AND_RESET);  // wait for the active prefetch to finish.
  size_t prefetch_res = prefetch_res_.load(std::memory_order_acquire);

  if (prefetch_res > 0) {
    if (prefetch_.empty()) {
      prefetch_ = MutableBytes(prefetch_ptr_, prefetch_res);
    } else {
      CHECK(prefetch_.end() == prefetch_ptr_);
      prefetch_ = MutableBytes(prefetch_.data(), prefetch_res + prefetch_.size());
    }
    DCHECK_LE(prefetch_.end() - buf_.get(), ptrdiff_t(buf_size_));
    if (stats_) {
      if (preempt) {
        if (VLOG_IS_ON(1)) {
          auto delta = absl::Now() - prefetch_start_ts_;
          tp_wait_hist_.Add(absl::ToInt64Microseconds(delta));
        }
        ++stats_->preempt_cnt;
        stats_->disk_bytes += prefetch_res;
      } else {
        stats_->cache_bytes += prefetch_res;
      }
    }
  } else {
    // We ignore the error, maximum the system will reread it in the through the main thread.
    file_prefetch_offset_ = -1;
  }
  prefetch_ptr_ = nullptr;
}

auto FiberReadFile::Read(size_t offset, const MutableBytes& range) -> SizeOrError {
  SizeOrError res;

  if (buf_) {  // prefetch enabled.
    res = ReadAndPrefetch(offset, range);
    VLOG(2) << "ReadAndPrefetch " << offset << "/" << res.value();
    return res;
  }

  tp_->Add([&] {
    res = next_->Read(offset, range);
    done_.Notify();
  });
  done_.Wait(AND_RESET);
  VLOG(1) << "Read " << offset << "/" << res.value();
  return res;
}

error_code WriteFileImpl::Close() {
  error_code res;
  if (!has_error_.load(std::memory_order_relaxed)) {
    if (upstream_) {
      // must be first to ensure all write operations finish.
      res = tp_->Await([this] { return upstream_->Close(); });
    }
  }
  // After the barrier passed we know all writes completed.
  if (!res) {
    unique_lock<fibers::mutex> lk(mu_);
    res = ec_;
  }
  return res;
}

error_code WriteFileImpl::Write(const uint8* buffer, uint64 length) {
  auto cb = [&] { return upstream_->Write(buffer, length); };
  if (hash_ < 0)
    return tp_->Await(std::move(cb));
  else
    return tp_->Await(hash_, std::move(cb));
}

void WriteFileImpl::AsyncWrite(std::string blob) {
  if (has_error_.load(std::memory_order_relaxed))
    return;  // Do not bother

  auto cb = [this, blob = std::move(blob)] {
    auto ec = upstream_->Write(blob);
    if (ec) {
      unique_lock<fibers::mutex> lk(mu_);
      ec_ = ec;
      has_error_.store(true, std::memory_order_relaxed);  // under mutex.
    }
  };

  if (hash_ < 0)
    tp_->Add(std::move(cb));
  else
    tp_->Add(hash_, std::move(cb));
}

}  // namespace

ReadonlyFileOrError OpenFiberReadFile(absl::string_view name, fibers_ext::FiberQueueThreadPool* tp,
                                      const FiberReadOptions& opts) {
  ReadonlyFileOrError res = OpenRead(name, opts);
  if (!res)
    return res;
  return new FiberReadFile(opts, res.value(), tp);
}

WriteFileOrError OpenFiberWriteFile(absl::string_view name, fibers_ext::FiberQueueThreadPool* tp,
                                    const FiberWriteOptions& opts) {
  WriteFileOrError res = OpenWrite(name, opts);
  if (!res)
    return res;

  ssize_t hash = -1;
  if (opts.consistent_thread)
    hash = base::XXHash32(name);
  return new WriteFileImpl(res.value(), hash, tp);
}

}  // namespace util
