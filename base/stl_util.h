// Copyright 2017, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include <algorithm>

#include <ostream>
#include <vector>

#include "base/integral_types.h"


template<typename T> std::ostream& operator<<(std::ostream& o, const std::vector<T>& vec) {
  o << "[";
  for (size_t i = 0; i < vec.size(); ++i) {
    o << vec[i];
    if (i + 1 < vec.size()) o << ",";
  }
  o << "]";
  return o;
}

namespace base {

template<typename T, typename U> bool _in(const T& t,
    std::initializer_list<U> l) {
  return std::find(l.begin(), l.end(), t) != l.end();
}


template<typename T> struct MinMax {

  T min_val = std::numeric_limits<T>::max(), max_val = std::numeric_limits<T>::min();

  void Update(T val) {
    min_val = std::min(min_val, val);
    max_val = std::max(max_val, val);
  }
};

}  // namespace base
