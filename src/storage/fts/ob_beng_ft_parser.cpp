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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "lib/allocator/page_arena.h"
#include "lib/thread_local/ob_tsi_factory.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  char *buf = nullptr;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(token_alloc_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buf = static_cast<char *>(token_alloc_->alloc(token.len_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, token.ptr_, token.len_);
    word = buf;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    token_alloc_ = param->allocator_;
    doc_.set_string(param->fulltext_, param->ft_length_);
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    if (OB_FAIL(english_analyzer_.init(analysis_ctx_, *param->allocator_))) {
    } else if (OB_FAIL(segment(doc_, token_stream_))) {
    } else if (OB_ISNULL(token_stream_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObBEngFTParser::reuse(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (analysis_ctx_.cs_ != param->cs_) {
    // Normalizers keep the charset pointer from init.
    ret = OB_NOT_SUPPORTED;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    token_alloc_ = param->allocator_;
    doc_.set_string(param->fulltext_, param->ft_length_);
    if (OB_FAIL(segment(doc_, token_stream_))) {
    }
  }
  return ret;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  doc_.reset();
  token_stream_ = nullptr;
  is_inited_ = false;
}

namespace
{
struct ObBEngParserSlot
{
  ObBEngParserSlot() : alloc(lib::ObMemAttr("BEngParser")), parser(nullptr), in_use(false) {}
  ~ObBEngParserSlot()
  {
    if (nullptr != parser) {
      parser->destroy();
      parser = nullptr;
    }
  }
  ObArenaAllocator alloc;
  ObBEngFTParser *parser;
  bool in_use;
};

// ObDITls deletes the slot at thread exit, not in static destruction order,
// and refuses to construct a new one once the thread is leaving.
typedef common::ObDITls<ObBEngParserSlot, 0x42454e31> ObBEngParserSlotTls;

ObBEngParserSlot *beng_parser_slot()
{
  return ObBEngParserSlotTls::get_instance();
}

void release_beng_parser(ObBEngFTParser *parser)
{
  ObBEngParserSlot *slot = beng_parser_slot();
  if (OB_NOT_NULL(slot) && slot->parser == parser) {
    slot->in_use = false;
  }
}

void destroy_beng_parser_slot()
{
  ObBEngParserSlot *slot = beng_parser_slot();
  if (OB_NOT_NULL(slot) && nullptr != slot->parser) {
    slot->parser->destroy();
    slot->alloc.reset();
    slot->parser = nullptr;
  }
  if (OB_NOT_NULL(slot)) {
    slot->in_use = false;
  }
}
} // namespace

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  bool pooled = false;
  bool need_init = true;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObBEngParserSlot *slot = beng_parser_slot();
    if (OB_ISNULL(slot)) {
      // Leaving the thread: no new thread-local parser is allowed.
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (!slot->in_use && nullptr != slot->parser && slot->parser->inited()
        && slot->parser->text_charset() == param->cs_) {
      slot->in_use = true;
      parser = slot->parser;
      pooled = true;
      need_init = false;
      if (OB_FAIL(parser->reuse(param))) {
        destroy_beng_parser_slot();
        parser = nullptr;
        pooled = false;
      }
    } else if (!slot->in_use) {
      if (nullptr != slot->parser) {
        destroy_beng_parser_slot();
      }
      if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, &slot->alloc, slot->alloc))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(parser->init(param))) {
        parser->~ObBEngFTParser();
        parser = nullptr;
      } else {
        parser->set_pooled(true);
        slot->parser = parser;
        slot->in_use = true;
        pooled = true;
        need_init = false;
      }
    } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }

  if (OB_FAIL(ret)) {
  } else if (need_init && OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret)) {
    if (pooled) {
      release_beng_parser(parser);
    } else if (OB_NOT_NULL(param) && OB_NOT_NULL(param->allocator_)) {
      OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
    }
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    ObBEngFTParser *parser = static_cast<ObBEngFTParser *>(iter);
    if (parser->is_pooled()) {
      release_beng_parser(parser);
    } else {
      abort_unless(nullptr != param);
      abort_unless(nullptr != param->allocator_);
      iter->~ObITokenIterator();
      param->allocator_->free(iter);
    }
    iter = nullptr;
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
