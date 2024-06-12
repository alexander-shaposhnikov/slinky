#ifndef SLINKY_BASE_REF_COUNT_H
#define SLINKY_BASE_REF_COUNT_H

#include <algorithm>
#include <atomic>

#include "base/util.h"

namespace slinky {

// Base class for reference counted objects.
template <typename T>
class ref_counted {
  mutable std::atomic<int> ref_count_{0};

public:
  int ref_count() const { return ref_count_; }
  void add_ref() const { ++ref_count_; }
  void release() const {
    if (--ref_count_ == 0) {
      // This const_cast is ugly, but:
      // https://stackoverflow.com/questions/755196/deleting-a-pointer-to-const-t-const
      T::destroy(const_cast<T*>(static_cast<const T*>(this)));
    }
  }

  virtual ~ref_counted() = default;
};

// A smart pointer to a ref_counted base.
template <typename T>
class ref_count {
  T* value;

public:
  ref_count(T* v = nullptr) : value(v) {
    if (value) value->add_ref();
  }
  ref_count(const ref_count& other) : ref_count(other.value) {}
  ref_count(ref_count&& other) noexcept : value(other.value) { other.value = nullptr; }
  ~ref_count() {
    if (value) value->release();
  }

  ref_count& operator=(T* v) {
    if (value != v) {
      std::swap(value, v);
      if (value) value->add_ref();
      if (v) v->release();
    }
    return *this;
  }

  ref_count& operator=(const ref_count& other) { return operator=(other.value); }

  ref_count& operator=(ref_count&& other) noexcept {
    std::swap(value, other.value);
    other = nullptr;
    return *this;
  }

  SLINKY_ALWAYS_INLINE T& operator*() { return *value; }
  SLINKY_ALWAYS_INLINE const T& operator*() const { return *value; }
  SLINKY_ALWAYS_INLINE T* operator->() { return value; }
  SLINKY_ALWAYS_INLINE const T* operator->() const { return value; }

  SLINKY_ALWAYS_INLINE operator T*() { return value; }
  SLINKY_ALWAYS_INLINE operator const T*() const { return value; }
};

}  // namespace slinky

#endif  // SLINKY_BASE_REF_COUNT_H
