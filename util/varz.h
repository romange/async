// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "base/arena.h"
#include "base/varz_node.h"
#include "util/sliding_counter.h"

#define DEFINE_VARZ(type, name) ::util::type name(#name)

namespace util {

class VarzQps : public base::VarzListNode {
 public:
  explicit VarzQps(const char* varname) : base::VarzListNode(varname) {
  }

  void Init(ProactorPool* pp) {
    val_.Init(pp);
  }

  void Shutdown() { val_.Shutdown(); }

  void Inc() {
    val_.Inc();
  }

 private:
  virtual AnyValue GetData() const override;

  using Counter = SlidingCounter<7>;

  // 7-seconds window. We gather data based on the fully filled 6.
  Counter val_;
};

class VarzMapAverage : public base::VarzListNode {
  using Counter = SlidingCounterTL<7>;
  using SumCnt = std::pair<Counter, Counter>;
  using Map = absl::flat_hash_map<absl::string_view, SumCnt>;

 public:
  explicit VarzMapAverage(const char* varname) : base::VarzListNode(varname) {
  }
  ~VarzMapAverage();

  void Init(ProactorPool* pp);
  void Shutdown();

  void IncBy(absl::string_view key, int32_t delta) {
    auto& map = avg_map_[ProactorThreadIndex()];
    auto it = map.find(key);
    if (it == map.end()) {
      it = FindSlow(key);
    }
    Inc(delta, &it->second);
  }

 private:
  void Inc(int32_t delta, SumCnt* dest) {
    dest->first.IncBy(delta);
    dest->second.Inc();
  }

  virtual AnyValue GetData() const override;
  unsigned ProactorThreadIndex() const;
  Map::iterator FindSlow(absl::string_view key);

  ProactorPool* pp_ = nullptr;
  std::unique_ptr<Map[]> avg_map_;
};


class VarzCount : public base::VarzListNode {

 public:
  explicit VarzCount(const char* varname) : base::VarzListNode(varname) {
  }
  ~VarzCount();

  void Init(ProactorPool* pp);
  void Shutdown();

  void IncBy(int64_t delta) {
    count_.fetch_add(delta, std::memory_order_relaxed);
  }

 private:
  AnyValue GetData() const override;

  std::atomic_int64_t count_{0};
};

class VarzFunction : public base::VarzListNode {
 public:
  typedef AnyValue::Map KeyValMap;
  typedef std::function<KeyValMap()> MapCb;

  // cb - function that formats the output either as json or html according to the boolean is_json.
  explicit VarzFunction(const char* varname, MapCb cb) : VarzListNode(varname), cb_(cb) {
  }

 private:
  AnyValue GetData() const override;

  MapCb cb_;
};

}  // namespace util
