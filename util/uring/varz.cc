// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/varz.h"

using namespace boost;

using base::VarzValue;

namespace util {
namespace uring {

VarzValue VarzQps::GetData() const {

  uint32_t qps =  val_.SumTail() / (Counter::WIN_SIZE - 1); // Average over kWinSize values.
  return VarzValue::FromInt(qps);
}

VarzMapAverage::~VarzMapAverage() {}

void VarzMapAverage::Init(ProactorPool* pp) {
  CHECK(pp_ == nullptr);
  pp_ = CHECK_NOTNULL(pp);
  avg_map_.reset(new Map[pp->size()]);
}

unsigned VarzMapAverage::ProactorThreadIndex() const {
  unsigned tnum = CHECK_NOTNULL(pp_)->size();

  int32_t indx = Proactor::GetIndex();
  CHECK_GE(indx, 0) << "Must be called from proactor thread!";
  CHECK_LT(unsigned(indx), tnum) << "Invalid thread index " << indx;

  return unsigned(indx);
}

auto VarzMapAverage::FindSlow(absl::string_view key) -> Map::iterator {
  auto str = pp_->GetString(key);
  auto& map = avg_map_[Proactor::GetIndex()];
  auto res = map.emplace(str, SumCnt{});

  CHECK(res.second);
  return res.first;
}

VarzValue VarzMapAverage::GetData() const {
  CHECK(pp_);

  AnyValue::Map result;
  fibers::mutex mu;

  auto cb = [&](unsigned index, Proactor*) {
    auto& map = avg_map_[index];

    for (const auto& k_v : map) {
      AnyValue::Map items;
      int64 count = k_v.second.second.Sum();
      int64 sum = k_v.second.first.Sum();
      items.emplace_back("count", VarzValue::FromInt(count));
      items.emplace_back("sum", VarzValue::FromInt(sum));

      if (count) {
        double avg = count > 0 ? double(sum) / count : 0;
        items.emplace_back("average", VarzValue::FromDouble(avg));
      }

      std::unique_lock<fibers::mutex> lk(mu);
      result.emplace_back(std::string(k_v.first), std::move(items));
    }
  };
  pp_->AwaitFiberOnAll(cb);

  return result;
}

VarzValue VarzFunction::GetData() const {
  AnyValue::Map result = cb_();
  return AnyValue(result);
}

}
}  // namespace util
