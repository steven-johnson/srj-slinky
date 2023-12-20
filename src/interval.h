#ifndef SLINKY_INTERVAL_H
#define SLINKY_INTERVAL_H

#include "expr.h"

namespace slinky {

struct interval {
  expr min, max;

  interval() {}
  explicit interval(const expr& point) : min(point), max(point) {}
  interval(expr min, expr max) : min(std::move(min)), max(std::move(max)) {}

  expr extent() const {
    return max - min + 1;
  }
  void set_extent(expr extent) {
    max = min + extent - 1;
  }

  interval& operator*=(expr scale) {
    min *= scale;
    max *= scale;
    return *this;
  }

  interval& operator/=(expr scale) {
    min /= scale;
    max /= scale;
    return *this;
  }

  interval& operator+=(expr offset) {
    min += offset;
    max += offset;
    return *this;
  }

  interval& operator-=(expr offset) {
    min -= offset;
    max -= offset;
    return *this;
  }

  // This is the union operator. I don't really like this, but
  // I also don't like that I can't name a function `union`.
  // It does kinda make sense...
  interval& operator|=(const interval& r) {
    min = slinky::min(min, r.min);
    max = slinky::max(max, r.max);
    return *this;
  }

  // This is intersection, just to be consistent with union.
  interval& operator&=(const interval& r) {
    min = slinky::min(min, r.min);
    max = slinky::max(max, r.max);
    return *this;
  }

  interval operator*(expr scale) const {
    interval result(*this);
    result *= scale;
    return result;
  }

  interval operator/(expr scale) const {
    interval result(*this);
    result /= scale;
    return result;
  }

  interval operator+(expr offset) const {
    interval result(*this);
    result += offset;
    return result;
  }

  interval operator-(expr offset) const {
    interval result(*this);
    result -= offset;
    return result;
  }

  interval operator|(const interval& r) const {
    interval result(*this);
    result |= r;
    return result;
  }

  interval operator&(const interval& r) const {
    interval result(*this);
    result &= r;
    return result;
  }
};

using box = std::vector<interval>;

inline box operator|(box a, const box& b) {
  assert(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i] |= b[i];
  }
  return a;
}

inline box operator&(box a, const box& b) {
  assert(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i] &= b[i];
  }
  return a;
}

}  // namespace slinky

#endif  // SLINKY_INTERVAL_H
