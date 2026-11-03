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

#include "data_plane/fts/ob_fts_struct.h"

#include "lib/charset/ob_charset.h"
#include "share/datum/ob_datum_funcs.h"
#include "storage/ob_storage_util.h"

namespace oceanbase
{
namespace storage
{

int ObFTWord::hash(uint64_t &hash_val) const
{
  int ret = OB_SUCCESS;
  common::ObDatumBasicFuncs *funcs = ObDatumFuncs::get_basic_func(meta_.get_type(), meta_.get_collation_type());
  if (OB_ISNULL(funcs)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (funcs->default_hash_ == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = funcs->default_hash_(word_, 0, hash_val, nullptr);
  }
  return ret;
}
namespace
{
struct WordMapSlot
{
  ObFTWordMap map;
  bool ready;
  bool busy;
  WordMapSlot() : map(), ready(false), busy(false) {}
  ~WordMapSlot()
  {
    if (map.created()) {
      map.destroy();
    }
  }
};

WordMapSlot &word_map_slot()
{
  static thread_local WordMapSlot slot;
  return slot;
}
} // namespace

ObFTWordMapPoolGuard::~ObFTWordMapPoolGuard()
{
  if (pooled_) {
    release(map_);
  } else if (inited_ && local_.created()) {
    local_.destroy();
  }
}

int ObFTWordMapPoolGuard::init(const int64_t bucket_cnt)
{
  int ret = OB_SUCCESS;
  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(acquire(bucket_cnt, map_, pooled_))) {
  } else if (pooled_) {
    inited_ = true;
  } else if (OB_FAIL(local_.create(bucket_cnt > 0 ? bucket_cnt : 2, common::ObMemAttr("FTWordMap")))) {
  } else {
    map_ = &local_;
    inited_ = true;
  }
  return ret;
}

int ObFTWordMapPoolGuard::acquire(const int64_t bucket_cnt, ObFTWordMap *&map, bool &pooled)
{
  int ret = OB_SUCCESS;
  pooled = false;
  map = nullptr;
  WordMapSlot &slot = word_map_slot();
  if (slot.busy) {
    // Nested segmentation uses a private map.
  } else if (!slot.ready) {
    const int64_t requested = bucket_cnt > 0 ? bucket_cnt : 2;
    const int64_t buckets = requested < 997 ? 997 : requested;
    if (OB_FAIL(slot.map.create(buckets, common::ObMemAttr("FTWordMap")))) {
    } else {
      slot.ready = true;
      slot.busy = true;
      pooled = true;
      map = &slot.map;
    }
  } else if (OB_FAIL(slot.map.reuse())) {
  } else {
    slot.busy = true;
    pooled = true;
    map = &slot.map;
  }
  return ret;
}

void ObFTWordMapPoolGuard::release(ObFTWordMap *map)
{
  WordMapSlot &slot = word_map_slot();
  if (map == &slot.map) {
    slot.busy = false;
  }
}

bool ObFTWord::operator==(const ObFTWord &other) const
{
  bool is_equal = false;
  int ret = OB_SUCCESS;
  int cmp_ret = 0;
  ObDatumCmpFuncType func = get_datum_cmp_func(meta_, other.meta_);
  if (func == nullptr) {
    ob_abort();
  } else if (OB_FAIL(func(word_, other.word_, cmp_ret, nullptr))) {
    ob_abort();
  } else {
    is_equal = (cmp_ret == 0);
  }
  return is_equal;
}
} // namespace storage
} // namespace oceanbase
