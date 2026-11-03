/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_FT_SEGMENT_ARRAY_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_FT_SEGMENT_ARRAY_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace storage
{

// Growable ring buffer for IK token / hit lists.
// reset() keeps the backing storage so the next document reuses it.
// Logical index 0 is the front. push_front / pop_front are O(1).
template <typename T>
class ObFastSegmentArray
{
public:
  class iterator
  {
  public:
    iterator() : arr_(nullptr), idx_(-1) {}
    iterator(ObFastSegmentArray *arr, int64_t idx) : arr_(arr), idx_(idx) {}
    T &operator*() const { return arr_->at(idx_); }
    T *operator->() const { return &arr_->at(idx_); }
    iterator &operator++()
    {
      if (nullptr != arr_ && idx_ >= 0 && idx_ + 1 < arr_->size_) {
        ++idx_;
      } else {
        idx_ = -1;
      }
      return *this;
    }
    iterator operator++(int)
    {
      iterator prev = *this;
      ++(*this);
      return prev;
    }
    iterator &operator--()
    {
      if (nullptr == arr_) {
        idx_ = -1;
      } else if (idx_ < 0) {
        idx_ = arr_->size_ - 1;
      } else if (0 == idx_) {
        idx_ = -1;
      } else {
        --idx_;
      }
      return *this;
    }
    bool operator==(const iterator &other) const { return idx_ == other.idx_; }
    bool operator!=(const iterator &other) const { return idx_ != other.idx_; }
    int64_t index() const { return idx_; }
  private:
    ObFastSegmentArray *arr_;
    int64_t idx_;
  };

  class const_iterator
  {
  public:
    const_iterator() : arr_(nullptr), idx_(-1) {}
    const_iterator(const ObFastSegmentArray *arr, int64_t idx) : arr_(arr), idx_(idx) {}
    const T &operator*() const { return arr_->at(idx_); }
    const T *operator->() const { return &arr_->at(idx_); }
    const_iterator &operator++()
    {
      if (nullptr != arr_ && idx_ >= 0 && idx_ + 1 < arr_->size_) {
        ++idx_;
      } else {
        idx_ = -1;
      }
      return *this;
    }
    const_iterator operator++(int)
    {
      const_iterator prev = *this;
      ++(*this);
      return prev;
    }
    bool operator==(const const_iterator &other) const { return idx_ == other.idx_; }
    bool operator!=(const const_iterator &other) const { return idx_ != other.idx_; }
    bool operator==(const iterator &other) const { return idx_ == other.index(); }
    bool operator!=(const iterator &other) const { return idx_ != other.index(); }
  private:
    const ObFastSegmentArray *arr_;
    int64_t idx_;
  };

  explicit ObFastSegmentArray(common::ObIAllocator &alloc)
      : alloc_(alloc), buf_(nullptr), cap_(0), head_(0), size_(0)
  {
  }
  ~ObFastSegmentArray() { buf_ = nullptr; cap_ = 0; head_ = 0; size_ = 0; }
  ObFastSegmentArray(const ObFastSegmentArray &) = delete;
  ObFastSegmentArray &operator=(const ObFastSegmentArray &) = delete;

  int push_back(const T &value)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(ensure(1))) {
    } else {
      store(buf_[physical(size_)], value);
      ++size_;
    }
    return ret;
  }

  int push_front(const T &value)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(ensure(1))) {
    } else {
      head_ = (head_ + cap_ - 1) % cap_;
      store(buf_[head_], value);
      ++size_;
    }
    return ret;
  }

  int pop_back()
  {
    int ret = OB_SUCCESS;
    if (size_ <= 0) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      --size_;
      if (0 == size_) {
        head_ = 0;
      }
    }
    return ret;
  }

  int pop_front()
  {
    int ret = OB_SUCCESS;
    if (size_ <= 0) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      head_ = (head_ + 1) % cap_;
      --size_;
      if (0 == size_) {
        head_ = 0;
      }
    }
    return ret;
  }

  int pop_front(T &value)
  {
    int ret = OB_SUCCESS;
    if (size_ <= 0) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      store(value, at(0));
      ret = pop_front();
    }
    return ret;
  }

  // Insert before iter. end() appends.
  int insert(iterator iter, const T &value)
  {
    int ret = OB_SUCCESS;
    if (iter.index() < 0) {
      ret = push_back(value);
    } else {
      ret = insert_before(iter.index(), value);
    }
    return ret;
  }

  int remove_at(const int64_t index)
  {
    int ret = OB_SUCCESS;
    if (index < 0 || index >= size_) {
      ret = OB_ARRAY_OUT_OF_RANGE;
    } else {
      for (int64_t i = index; i + 1 < size_; ++i) {
        store(at(i), at(i + 1));
      }
      --size_;
      if (0 == size_) {
        head_ = 0;
      }
    }
    return ret;
  }

  T &get_first() { return at(0); }
  const T &get_first() const { return at(0); }
  T &get_last() { return at(size_ - 1); }
  const T &get_last() const { return at(size_ - 1); }

  T &at(const int64_t index) { return buf_[physical(index)]; }
  const T &at(const int64_t index) const { return buf_[physical(index)]; }

  bool empty() const { return 0 == size_; }
  int64_t size() const { return size_; }
  int64_t count() const { return size_; }

  void reset()
  {
    size_ = 0;
    head_ = 0;
  }
  void clear() { reset(); }

  iterator begin() { return size_ > 0 ? iterator(this, 0) : end(); }
  iterator end() { return iterator(this, -1); }
  iterator last() { return size_ > 0 ? iterator(this, size_ - 1) : end(); }
  const_iterator begin() const { return size_ > 0 ? const_iterator(this, 0) : end(); }
  const_iterator end() const { return const_iterator(this, -1); }

private:
  // The copy constructor keeps ObDATrieHit::dict_, which its operator= drops.
  // A memcpy would overwrite a live object's bytes behind its back.
  static void store(T &dst, const T &src)
  {
    if (&dst != &src) {
      new (&dst) T(src);
    }
  }

  int64_t physical(const int64_t logical) const { return (head_ + logical) % cap_; }

  int ensure(const int64_t need)
  {
    int ret = OB_SUCCESS;
    if (size_ + need <= cap_) {
    } else {
      int64_t ncap = cap_ <= 0 ? 32 : cap_;
      while (ncap < size_ + need) {
        if (ncap > INT64_MAX / 2) {
          ret = OB_SIZE_OVERFLOW;
          break;
        }
        ncap *= 2;
      }
      T *nbuf = nullptr;
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(nbuf = static_cast<T *>(alloc_.alloc(sizeof(T) * ncap)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (int64_t i = 0; i < size_; ++i) {
          store(nbuf[i], at(i));
        }
        buf_ = nbuf;
        cap_ = ncap;
        head_ = 0;
      }
    }
    return ret;
  }

  int linearize()
  {
    int ret = OB_SUCCESS;
    if (0 == head_ || 0 == size_) {
      head_ = 0;
    } else {
      T *nbuf = nullptr;
      if (OB_ISNULL(nbuf = static_cast<T *>(alloc_.alloc(sizeof(T) * cap_)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (int64_t i = 0; i < size_; ++i) {
          store(nbuf[i], at(i));
        }
        buf_ = nbuf;
        head_ = 0;
      }
    }
    return ret;
  }

  int insert_before(const int64_t index, const T &value)
  {
    int ret = OB_SUCCESS;
    if (index < 0 || index > size_) {
      ret = OB_INVALID_ARGUMENT;
    } else if (index == size_) {
      ret = push_back(value);
    } else if (0 == index) {
      ret = push_front(value);
    } else if (OB_FAIL(ensure(1))) {
    } else if (0 != head_ && OB_FAIL(linearize())) {
    } else {
      for (int64_t i = size_; i > index; --i) {
        store(buf_[i], buf_[i - 1]);
      }
      store(buf_[index], value);
      ++size_;
    }
    return ret;
  }

private:
  common::ObIAllocator &alloc_;
  T *buf_;
  int64_t cap_;
  int64_t head_;
  int64_t size_;
};

} // namespace storage
} // namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_FT_SEGMENT_ARRAY_H_
