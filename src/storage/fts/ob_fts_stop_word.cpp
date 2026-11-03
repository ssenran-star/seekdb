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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "lib/charset/ob_charset.h"
#include "lib/charset/ob_ctype.h"
#include "lib/thread_local/ob_tsi_factory.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

namespace
{
bool is_ascii_compatible_charset(const common::ObCharsetType charset)
{
  switch (charset) {
  case common::CHARSET_UTF8MB4:
  case common::CHARSET_GBK:
  case common::CHARSET_GB18030:
  case common::CHARSET_GB18030_2022:
  case common::CHARSET_LATIN1:
  case common::CHARSET_ASCII:
  case common::CHARSET_BINARY:
  case common::CHARSET_GB2312:
  case common::CHARSET_TIS620:
  case common::CHARSET_DEC8:
  case common::CHARSET_SWE7:
  case common::CHARSET_CP850:
    return true;
  default:
    return false;
  }
}

// ObDITls destroys at thread exit instead of at static destruction order, and
// refuses to construct once the thread is leaving.
typedef common::ObDITls<common::ObArenaAllocator, 0x53543331> ObStopWordConvertArena;

common::ObArenaAllocator *stopword_convert_allocator()
{
  return ObStopWordConvertArena::get_instance();
}

void resolve_stopword_lookup(const int exist_ret, bool &is_stopword, int &ret)
{
  if (OB_HASH_NOT_EXIST == exist_ret) {
    is_stopword = false;
    ret = OB_SUCCESS;
  } else if (OB_HASH_EXIST == exist_ret) {
    is_stopword = true;
    ret = OB_SUCCESS;
  } else if (OB_SUCC(exist_ret)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = exist_ret;
  }
}
} // namespace

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const common::ObString raw = word.get_word().get_string();
    const common::ObCharsetType charset = common::ObCharset::charset_type_by_coll(word.get_collation_type());
    bool ascii = is_ascii_compatible_charset(charset);
    for (int64_t i = 0; ascii && i < raw.length(); ++i) {
      if (static_cast<unsigned char>(raw.ptr()[i]) >= 0x80) {
        ascii = false;
      }
    }
    if (ascii && raw.length() > FTS_STOP_WORD_MAX_LENGTH) {
      // Built-in stopwords are short ASCII. A longer ASCII token cannot fold into one.
      is_stopword = false;
    } else if (ascii) {
      char folded[FTS_STOP_WORD_MAX_LENGTH];
      const int64_t len = raw.length();
      for (int64_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw.ptr()[i]);
        folded[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
      }
      ObFTWord converted(len, folded, stopword_type_);
      resolve_stopword_lookup(stopword_set_.exist_refactored(converted), is_stopword, ret);
    } else {
      // One arena per thread. The previous implementation allocated a new one per token.
      common::ObArenaAllocator *allocator = stopword_convert_allocator();
      common::ObString cmp_str;
      if (OB_ISNULL(allocator)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (FALSE_IT(allocator->reuse())) {
      } else if (OB_FAIL(common::ObCharset::charset_convert(
                                         *allocator,
                                         raw,
                                         word.get_collation_type(),
                                         stopword_type_.get_collation_type(),
                                         cmp_str))) {
      } else {
        ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
        resolve_stopword_lookup(stopword_set_.exist_refactored(converted), is_stopword, ret);
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    const ObString in = src.get_word().get_string();
    const ObCollationType coll = word_meta_.get_collation_type();
    const ObCharsetInfo *cs = ObCharset::get_charset(coll);
    bool stored = false;
    // utf8mb4 casedn_multiply is 1, so tolower used to memcpy every token
    // before folding. Skip the heap copy when folding does not change bytes.
    static const int64_t STACK_CASE_BUF = 256;
    if (OB_NOT_NULL(cs) && OB_NOT_NULL(cs->cset) && OB_NOT_NULL(cs->cset->casedn)
        && 1 == cs->casedn_multiply && in.length() <= STACK_CASE_BUF) {
      char buf[STACK_CASE_BUF];
      const size_t size = cs->cset->casedn(cs,
                                           const_cast<char *>(in.ptr()),
                                           static_cast<size_t>(in.length()),
                                           buf,
                                           static_cast<size_t>(in.length()));
      if (size == static_cast<size_t>(in.length()) && 0 == MEMCMP(buf, in.ptr(), size)) {
        dst = src;
        stored = true;
      } else if (size > 0 && size <= static_cast<size_t>(in.length())) {
        char *copied = static_cast<char *>(allocator_.alloc(static_cast<int64_t>(size)));
        if (OB_ISNULL(copied)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(copied, buf, size);
          ObFTWord tmp(static_cast<int64_t>(size), copied, word_meta_);
          dst = tmp;
          stored = true;
        }
      }
    }
    if (OB_SUCC(ret) && !stored) {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(coll, in, dst_str, allocator_))) {
      } else {
        ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
        dst = tmp;
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  int64_t word_count = 0;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else if (OB_FAIL(word_map_.get_refactored(word, word_count)) && OB_HASH_NOT_EXIST != ret) {
  } else {
    if (OB_HASH_NOT_EXIST == ret) {
      word_count = 1;
    } else {
      word_count += word_freq;
    }
    if (OB_FAIL(word_map_.set_refactored(word, word_count, 1/*overwrite*/))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
