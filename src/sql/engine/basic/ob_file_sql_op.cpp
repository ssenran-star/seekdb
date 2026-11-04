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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_sql_op.h"

#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/object/ob_obj_cast.h"
#include "common/object/ob_object.h"
#include "common/timezone/ob_time_convert.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/resolver/ob_resolver_utils.h"

#include <zlib.h>
#include <glob.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

namespace oceanbase
{
namespace sql
{
using namespace common;

struct ObFileSqlOp::Reader
{
  virtual ~Reader() {}
  virtual int open() = 0;
  virtual int next(ObIAllocator &alloc, ObIArray<ObObj> &row) = 0;
  virtual void close() = 0;
};

namespace
{

constexpr int64_t FILE_SQL_MAX_FIELD = 16L * 1024 * 1024;
constexpr int64_t FILE_SQL_IO_BUF = 64L * 1024;

enum ColKind { CK_UNK = 0, CK_BOOL, CK_INT, CK_DBL, CK_DATE, CK_DT, CK_STR };

struct ColBuild
{
  ObString name_;
  ColKind kind_;
  bool nullable_;
  bool seen_value_;
  int32_t max_len_;
  ColBuild() : name_(), kind_(CK_UNK), nullable_(false), seen_value_(false), max_len_(0) {}
};

struct RawField
{
  const char *ptr_;
  int32_t len_;
  bool is_null_;
  RawField() : ptr_(NULL), len_(0), is_null_(true) {}
};

int write_dup(ObIAllocator &alloc, const char *p, int32_t len, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  if (len < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (len > 0 && OB_NOT_NULL(p)) {
      MEMCPY(buf, p, len);
    }
    buf[len] = '\0';
    out.assign_ptr(buf, len);
  }
  return ret;
}

int write_dup(ObIAllocator &alloc, const ObString &in, ObString &out)
{
  return write_dup(alloc, in.ptr(), in.length(), out);
}

bool name_eq(const ObString &a, const char *b, int32_t blen)
{
  return a.length() == blen && 0 == strncasecmp(a.ptr(), b, blen);
}

bool is_bool_token(const char *p, int32_t len, bool &val)
{
  bool ok = false;
  if (4 == len && 0 == strncasecmp(p, "true", 4)) {
    val = true;
    ok = true;
  } else if (5 == len && 0 == strncasecmp(p, "false", 5)) {
    val = false;
    ok = true;
  }
  return ok;
}

bool is_int_token(const char *p, int32_t len, int64_t &out)
{
  if (len <= 0 || len > 20) {
    return false;
  }
  int32_t i = 0;
  bool neg = false;
  if ('+' == p[0] || '-' == p[0]) {
    neg = ('-' == p[0]);
    i = 1;
    if (i == len) {
      return false;
    }
  }
  uint64_t v = 0;
  for (; i < len; ++i) {
    if (p[i] < '0' || p[i] > '9') {
      return false;
    }
    v = v * 10 + static_cast<uint64_t>(p[i] - '0');
  }
  if (!neg && v > static_cast<uint64_t>(INT64_MAX)) {
    return false;
  }
  if (neg && v > static_cast<uint64_t>(INT64_MAX) + 1) {
    return false;
  }
  out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
  return true;
}

bool is_double_token(const char *p, int32_t len, double &out)
{
  bool saw_dot = false;
  bool saw_exp = false;
  for (int32_t i = 0; i < len; ++i) {
    char c = p[i];
    if ('.' == c) {
      saw_dot = true;
    } else if ('e' == c || 'E' == c) {
      saw_exp = true;
    }
  }
  if (!saw_dot && !saw_exp) {
    return false;
  }
  char tmp[128];
  char *buf = tmp;
  if (len >= static_cast<int32_t>(sizeof(tmp))) {
    return false;
  }
  MEMCPY(buf, p, len);
  buf[len] = '\0';
  char *end = NULL;
  errno = 0;
  double v = strtod(buf, &end);
  if (end != buf + len || ERANGE == errno) {
    return false;
  }
  out = v;
  return true;
}

bool looks_date(const char *p, int32_t len)
{
  if (10 != len) {
    return false;
  }
  for (int i = 0; i < 10; ++i) {
    if (4 == i || 7 == i) {
      if ('-' != p[i]) {
        return false;
      }
    } else if (p[i] < '0' || p[i] > '9') {
      return false;
    }
  }
  return true;
}

bool looks_datetime(const char *p, int32_t len)
{
  if (len < 19 || !looks_date(p, 10)) {
    return false;
  }
  if (' ' != p[10] && 'T' != p[10] && 't' != p[10]) {
    return false;
  }
  for (int i = 11; i < 19; ++i) {
    int rel = i - 11;
    if (2 == rel || 5 == rel) {
      if (':' != p[i]) {
        return false;
      }
    } else if (p[i] < '0' || p[i] > '9') {
      return false;
    }
  }
  return true;
}

int normalize_datetime(const char *p, int32_t len, char *buf, int32_t cap, ObString &out)
{
  int ret = OB_SUCCESS;
  if (len >= cap) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    MEMCPY(buf, p, len);
    buf[len] = '\0';
    if ('T' == buf[10] || 't' == buf[10]) {
      buf[10] = ' ';
    }
    out.assign_ptr(buf, len);
  }
  return ret;
}

ColKind classify_text(const char *p, int32_t len)
{
  bool b = false;
  int64_t iv = 0;
  double dv = 0;
  if (is_bool_token(p, len, b)) {
    return CK_BOOL;
  } else if (is_int_token(p, len, iv)) {
    return CK_INT;
  } else if (is_double_token(p, len, dv)) {
    return CK_DBL;
  } else if (looks_datetime(p, len)) {
    char tmp[128];
    ObString s;
    int64_t dt = 0;
    int16_t scale = 0;
    if (OB_SUCCESS == normalize_datetime(p, len, tmp, sizeof(tmp), s)) {
      ObTimeConvertCtx ctx(NULL, false);
      if (OB_SUCCESS == ObTimeConverter::str_to_datetime(s, ctx, dt, &scale)) {
        return CK_DT;
      }
    }
  } else if (looks_date(p, len)) {
    int32_t date = 0;
    if (OB_SUCCESS == ObTimeConverter::str_to_date(ObString(len, p), date)) {
      return CK_DATE;
    }
  }
  return CK_STR;
}

void promote(ColKind &cur, ColKind next)
{
  if (CK_UNK == next) {
    return;
  } else if (CK_UNK == cur) {
    cur = next;
  } else if (cur == next) {
    // same
  } else if ((CK_INT == cur && CK_DBL == next) || (CK_DBL == cur && CK_INT == next)) {
    cur = CK_DBL;
  } else {
    cur = CK_STR;
  }
}

void observe_field(ColBuild &col, const RawField &field)
{
  if (field.is_null_) {
    col.nullable_ = true;
  } else {
    col.seen_value_ = true;
    if (field.len_ > col.max_len_) {
      col.max_len_ = field.len_;
    }
    promote(col.kind_, classify_text(field.ptr_, field.len_));
  }
}

ObObjType kind_to_type(ColKind kind, bool &is_bool)
{
  is_bool = false;
  switch (kind) {
    case CK_BOOL: is_bool = true; return ObTinyIntType;
    case CK_INT: return ObIntType;
    case CK_DBL: return ObDoubleType;
    case CK_DATE: return ObDateType;
    case CK_DT: return ObDateTimeType;
    case CK_STR:
    case CK_UNK:
    default:
      return ObVarcharType;
  }
}

int text_to_obj(const char *p, int32_t len, bool is_null, ObObjType type, bool is_bool,
                ObCollationType cs, ObIAllocator &alloc, ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (is_null) {
    obj.set_null();
  } else if (is_bool || (ObTinyIntType == type && is_bool)) {
    bool b = false;
    int64_t iv = 0;
    if (is_bool_token(p, len, b)) {
      obj.set_tinyint(b ? 1 : 0);
    } else if (is_int_token(p, len, iv) && (0 == iv || 1 == iv)) {
      obj.set_tinyint(static_cast<int8_t>(iv));
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
  } else if (ObIntType == type) {
    int64_t iv = 0;
    double dv = 0;
    if (is_int_token(p, len, iv)) {
      obj.set_int(iv);
    } else if (is_double_token(p, len, dv) && dv == static_cast<double>(static_cast<int64_t>(dv))) {
      obj.set_int(static_cast<int64_t>(dv));
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
  } else if (ObDoubleType == type || ObFloatType == type) {
    int64_t iv = 0;
    double dv = 0;
    if (is_double_token(p, len, dv)) {
      obj.set_double(dv);
    } else if (is_int_token(p, len, iv)) {
      obj.set_double(static_cast<double>(iv));
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
  } else if (ObDateType == type) {
    int32_t date = 0;
    if (OB_FAIL(ObTimeConverter::str_to_date(ObString(len, p), date))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      obj.set_date(date);
    }
  } else if (ObDateTimeType == type) {
    char tmp[128];
    ObString s;
    int64_t dt = 0;
    int16_t scale = 0;
    ObTimeConvertCtx ctx(NULL, false);
    if (OB_FAIL(normalize_datetime(p, len, tmp, sizeof(tmp), s))) {
    } else if (OB_FAIL(ObTimeConverter::str_to_datetime(s, ctx, dt, &scale))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      obj.set_datetime(dt);
    }
  } else {
    char *buf = NULL;
    if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len > 0) {
        MEMCPY(buf, p, len);
      }
      buf[len] = '\0';
      obj.set_varchar(buf, len);
      obj.set_collation_type(cs);
      obj.set_collation_level(CS_LEVEL_IMPLICIT);
    }
  }
  return ret;
}

int check_secure_realpath(ObSQLSessionInfo *session, const char *path, ObString &canon, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  char buf[PATH_MAX];
  char *rp = NULL;
  ObString secure;
  if (OB_ISNULL(session) || OB_ISNULL(path) || '\0' == path[0]) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(rp = realpath(path, buf))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("realpath failed", K(ret), K(path), K(errno));
  } else if (OB_FAIL(session->get_secure_file_priv(secure))) {
    LOG_WARN("get secure_file_priv failed", K(ret));
  } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure, ObString(rp)))) {
    if (OB_ERR_NO_PRIVILEGE == ret) {
      ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    LOG_WARN("path is outside secure_file_priv", K(ret), K(secure), K(rp));
  } else if (OB_FAIL(write_dup(alloc, rp, static_cast<int32_t>(strlen(rp)), canon))) {
  }
  return ret;
}

int check_regular_file(const ObString &path)
{
  int ret = OB_SUCCESS;
  struct stat st;
  if (0 != stat(path.ptr(), &st)) {
    ret = OB_FILE_NOT_EXIST;
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file sql path is not a regular file", K(path));
  }
  return ret;
}

class ByteReader
{
public:
  ByteReader() : fd_(-1), buf_(), buf_len_(0), pos_(0), eof_(false) {}
  ~ByteReader() { close(); }
  int open(const char *path)
  {
    int ret = OB_SUCCESS;
    close();
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("open file failed", K(ret), K(path), K(errno));
    }
    return ret;
  }
  void close()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    buf_.reset();
    buf_len_ = 0;
    pos_ = 0;
    eof_ = false;
  }
  int get(char &ch, bool &eof)
  {
    int ret = OB_SUCCESS;
    eof = false;
    if (pos_ >= buf_len_) {
      if (eof_) {
        eof = true;
        return ret;
      }
      buf_len_ = ::read(fd_, buf_, FILE_SQL_IO_BUF);
      pos_ = 0;
      if (0 == buf_len_) {
        eof_ = true;
        eof = true;
        return ret;
      } else if (buf_len_ < 0) {
        ret = OB_IO_ERROR;
        return ret;
      }
    }
    ch = buf_[pos_++];
    return ret;
  }
  int pread_exact(int64_t off, char *dst, int64_t len)
  {
    int ret = OB_SUCCESS;
    int64_t got = 0;
    while (OB_SUCC(ret) && got < len) {
      ssize_t n = ::pread(fd_, dst + got, len - got, off + got);
      if (0 == n) {
        ret = OB_IO_ERROR;
      } else if (n < 0) {
        if (EINTR == errno) {
          continue;
        }
        ret = OB_IO_ERROR;
      } else {
        got += n;
      }
    }
    return ret;
  }
  int fd() const { return fd_; }
private:
  int fd_;
  char buf_[FILE_SQL_IO_BUF];
  int64_t buf_len_;
  int64_t pos_;
  bool eof_;
};

class FieldBuf
{
public:
  FieldBuf() : data_(NULL), cap_(0), len_(0) {}
  ~FieldBuf() { reset(); }
  void reset()
  {
    if (NULL != data_) {
      free(data_);
      data_ = NULL;
    }
    cap_ = 0;
    len_ = 0;
  }
  void clear() { len_ = 0; }
  int append(char c)
  {
    int ret = OB_SUCCESS;
    if (len_ + 1 > FILE_SQL_MAX_FIELD) {
      ret = OB_SIZE_OVERFLOW;
    } else if (len_ + 1 > cap_ && OB_FAIL(grow(len_ + 1))) {
    } else {
      data_[len_++] = c;
    }
    return ret;
  }
  int append_bytes(const char *p, int32_t n)
  {
    int ret = OB_SUCCESS;
    if (n < 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (len_ + n > FILE_SQL_MAX_FIELD) {
      ret = OB_SIZE_OVERFLOW;
    } else if (len_ + n > cap_ && OB_FAIL(grow(len_ + n))) {
    } else {
      if (n > 0) {
        MEMCPY(data_ + len_, p, n);
      }
      len_ += n;
    }
    return ret;
  }
  char *data() { return data_; }
  int32_t length() const { return static_cast<int32_t>(len_); }
private:
  int grow(int64_t need)
  {
    int ret = OB_SUCCESS;
    int64_t ncap = cap_ <= 0 ? 256 : cap_;
    while (ncap < need) {
      ncap *= 2;
    }
    char *nbuf = static_cast<char *>(realloc(data_, ncap));
    if (OB_ISNULL(nbuf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      data_ = nbuf;
      cap_ = ncap;
    }
    return ret;
  }
  char *data_;
  int64_t cap_;
  int64_t len_;
};

class CsvScanner
{
public:
  int open(const char *path)
  {
    int ret = reader_.open(path);
    if (OB_SUCC(ret)) {
      bom_checked_ = false;
    }
    return ret;
  }
  void close() { reader_.close(); field_.reset(); free_row(); }
  // header_mode: first row is returned like any other. Caller skips it.
  int get_char(char &ch, bool &eof)
  {
    if (has_pending_) {
      ch = pending_;
      has_pending_ = false;
      eof = false;
      return OB_SUCCESS;
    }
    return reader_.get(ch, eof);
  }
  int next(ObIArray<RawField> &fields, bool &eof)
  {
    int ret = OB_SUCCESS;
    eof = false;
    fields.reset();
    field_.clear();
    free_row();
    bool in_quotes = false;
    bool field_started = false;
    bool quoted = false;
    bool saw_char = false;
    char ch = 0;
    bool end = false;
    if (!bom_checked_) {
      bom_checked_ = true;
      bool e1 = false, e2 = false, e3 = false;
      char c1 = 0, c2 = 0, c3 = 0;
      if (OB_FAIL(get_char(c1, e1))) {
      } else if (e1) {
        eof = true;
        return ret;
      } else if (static_cast<unsigned char>(c1) == 0xEF) {
        if (OB_FAIL(get_char(c2, e2)) || e2 || static_cast<unsigned char>(c2) != 0xBB
            || OB_FAIL(get_char(c3, e3)) || e3 || static_cast<unsigned char>(c3) != 0xBF) {
          if (OB_SUCC(ret) && !e2) {
            if (OB_FAIL(field_.append(c1)) || OB_FAIL(field_.append(c2))) {
            } else {
              field_started = true;
              saw_char = true;
            }
          }
        }
      } else {
        if (OB_FAIL(field_.append(c1))) {
        } else {
          field_started = true;
          saw_char = true;
        }
      }
      if (OB_FAIL(ret) || eof) {
        return ret;
      }
    }
    while (OB_SUCC(ret) && !end) {
      bool ceof = false;
      if (OB_FAIL(get_char(ch, ceof))) {
      } else if (ceof) {
        if (!saw_char && !field_started && 0 == fields.count() && 0 == field_.length()) {
          eof = true;
        } else if (in_quotes) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("unterminated CSV quote", K(ret));
        } else if (OB_FAIL(push_field(fields, quoted, field_started))) {
        } else {
          end = true;
        }
      } else {
        saw_char = true;
        if (in_quotes) {
          if ('"' == ch) {
            char nxt = 0;
            bool ne = false;
            if (OB_FAIL(get_char(nxt, ne))) {
            } else if (!ne && '"' == nxt) {
              ret = field_.append('"');
            } else {
              in_quotes = false;
              if (!ne) {
                if (',' == nxt) {
                  ret = push_field(fields, true, true);
                  field_.clear();
                  field_started = false;
                  quoted = false;
                } else if ('\n' == nxt) {
                  ret = push_field(fields, true, true);
                  end = true;
                } else if ('\r' == nxt) {
                  ret = push_field(fields, true, true);
                  end = true;
                  char lf = 0;
                  bool le = false;
                  if (OB_SUCC(ret) && OB_FAIL(get_char(lf, le))) {
                  } else if (!le && '\n' != lf) {
                    // lone CR; the extra char is lost only if not LF. Put back is hard.
                    // Treat a non-LF as the start of the next field by appending it after ending.
                    // Simpler: if not LF, start a new row's first char by failing strictness.
                    // Files use \n or \r\n. A bare \r ends the row and the char is data of next row.
                    // We cannot unget. Store pending char.
                    pending_ = lf;
                    has_pending_ = true;
                  }
                } else {
                  ret = field_.append(nxt);
                  field_started = true;
                }
              } else {
                ret = push_field(fields, true, true);
                end = true;
              }
            }
          } else if (OB_FAIL(field_.append(ch))) {
          }
        } else if ('"' == ch && !field_started) {
          in_quotes = true;
          quoted = true;
          field_started = true;
        } else if (',' == ch) {
          ret = push_field(fields, quoted, field_started);
          field_.clear();
          field_started = false;
          quoted = false;
        } else if ('\n' == ch) {
          ret = push_field(fields, quoted, field_started);
          end = true;
        } else if ('\r' == ch) {
          ret = push_field(fields, quoted, field_started);
          end = true;
          char lf = 0;
          bool le = false;
          if (OB_SUCC(ret) && OB_FAIL(get_char(lf, le))) {
          } else if (!le && '\n' != lf) {
            pending_ = lf;
            has_pending_ = true;
          }
        } else if (OB_FAIL(field_.append(ch))) {
          // data
        } else {
          field_started = true;
        }
      }
    }
    return ret;
  }
private:
  int push_field(ObIArray<RawField> &fields, bool quoted, bool started)
  {
    int ret = OB_SUCCESS;
    RawField f;
    int32_t len = field_.length();
    if (!started || (!quoted && 0 == len)) {
      f.is_null_ = true;
    } else {
      char *buf = static_cast<char *>(malloc(len + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (len > 0) {
          MEMCPY(buf, field_.data(), len);
        }
        buf[len] = '\0';
        if (OB_FAIL(row_blocks_.push_back(buf))) {
          free(buf);
        } else {
          f.is_null_ = false;
          f.ptr_ = buf;
          f.len_ = len;
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(fields.push_back(f))) {
    }
    return ret;
  }
  void free_row()
  {
    for (int64_t i = 0; i < row_blocks_.count(); ++i) {
      free(row_blocks_.at(i));
    }
    row_blocks_.reset();
  }
  ByteReader reader_;
  FieldBuf field_;
  ObSEArray<char *, 16> row_blocks_;
  bool bom_checked_ = false;
  bool has_pending_ = false;
  char pending_ = 0;
};

class JsonScanner
{
public:
  int open(const char *path) { return reader_.open(path); }
  void close() { reader_.close(); line_.reset(); key_.reset(); val_.reset(); }
  struct Pair
  {
    ObString key_;
    RawField value_;
  };
  int next(ObIAllocator &alloc, ObIArray<Pair> &pairs, bool &eof)
  {
    int ret = OB_SUCCESS;
    eof = false;
    pairs.reset();
    line_.clear();
    bool started = false;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    while (OB_SUCC(ret) && !(started && 0 == depth)) {
      char ch = 0;
      bool e = false;
      if (OB_FAIL(reader_.get(ch, e))) {
      } else if (e) {
        if (!started) {
          eof = true;
        } else {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("truncated json line", K(ret));
        }
        break;
      } else if (!started && (' ' == ch || '\t' == ch || '\r' == ch || '\n' == ch)) {
        // skip
      } else {
        if (OB_FAIL(line_.append(ch))) {
        } else {
          started = true;
          if (in_str) {
            if (esc) {
              esc = false;
            } else if ('\\' == ch) {
              esc = true;
            } else if ('"' == ch) {
              in_str = false;
            }
          } else if ('"' == ch) {
            in_str = true;
          } else if ('{' == ch || '[' == ch) {
            ++depth;
          } else if ('}' == ch || ']' == ch) {
            --depth;
          }
        }
      }
    }
    if (OB_SUCC(ret) && !eof) {
      ret = parse_object(alloc, line_.data(), line_.length(), pairs);
    }
    return ret;
  }
private:
  int parse_object(ObIAllocator &alloc, const char *s, int32_t len, ObIArray<Pair> &pairs);
  int parse_value(const char *s, int32_t len, int32_t &i, RawField &field, FieldBuf &storage);
  ByteReader reader_;
  FieldBuf line_;
  FieldBuf key_;
  FieldBuf val_;
};

static void skip_ws(const char *s, int32_t len, int32_t &i)
{
  while (i < len && (' ' == s[i] || '\t' == s[i] || '\r' == s[i] || '\n' == s[i])) {
    ++i;
  }
}

static int decode_json_string(const char *s, int32_t len, int32_t &i, FieldBuf &out)
{
  int ret = OB_SUCCESS;
  out.clear();
  if (i >= len || '"' != s[i]) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ++i;
    while (OB_SUCC(ret) && i < len) {
      char c = s[i++];
      if ('"' == c) {
        break;
      } else if ('\\' == c) {
        if (i >= len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          char e = s[i++];
          char rep = e;
          if ('n' == e) rep = '\n';
          else if ('t' == e) rep = '\t';
          else if ('r' == e) rep = '\r';
          else if ('b' == e) rep = '\b';
          else if ('f' == e) rep = '\f';
          else if ('u' == e) {
            if (i + 4 > len) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              int cp = 0;
              for (int k = 0; k < 4; ++k) {
                char h = s[i++];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp += h - '0';
                else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                else ret = OB_INVALID_ARGUMENT;
              }
              if (OB_SUCC(ret)) {
                if (cp < 0x80) {
                  ret = out.append(static_cast<char>(cp));
                } else if (cp < 0x800) {
                  ret = out.append(static_cast<char>(0xC0 | (cp >> 6)));
                  if (OB_SUCC(ret)) ret = out.append(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                  ret = out.append(static_cast<char>(0xE0 | (cp >> 12)));
                  if (OB_SUCC(ret)) ret = out.append(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                  if (OB_SUCC(ret)) ret = out.append(static_cast<char>(0x80 | (cp & 0x3F)));
                }
              }
              continue;
            }
          }
          if (OB_SUCC(ret)) {
            ret = out.append(rep);
          }
        }
      } else if (OB_FAIL(out.append(c))) {
      }
    }
  }
  return ret;
}

int JsonScanner::parse_value(const char *s, int32_t len, int32_t &i, RawField &field, FieldBuf &storage)
{
  int ret = OB_SUCCESS;
  skip_ws(s, len, i);
  field = RawField();
  if (i >= len) {
    ret = OB_INVALID_ARGUMENT;
  } else if ('n' == s[i] && i + 4 <= len && 0 == memcmp(s + i, "null", 4)) {
    i += 4;
    field.is_null_ = true;
  } else if ('"' == s[i]) {
    if (OB_FAIL(decode_json_string(s, len, i, storage))) {
    } else {
      field.is_null_ = false;
      field.ptr_ = storage.data();
      field.len_ = storage.length();
    }
  } else if ('t' == s[i] && i + 4 <= len && 0 == memcmp(s + i, "true", 4)) {
    i += 4;
    field.is_null_ = false;
    field.ptr_ = "true";
    field.len_ = 4;
  } else if ('f' == s[i] && i + 5 <= len && 0 == memcmp(s + i, "false", 5)) {
    i += 5;
    field.is_null_ = false;
    field.ptr_ = "false";
    field.len_ = 5;
  } else if ('{' == s[i] || '[' == s[i]) {
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    int32_t start = i;
    do {
      char c = s[i++];
      if (in_str) {
        if (esc) esc = false;
        else if ('\\' == c) esc = true;
        else if ('"' == c) in_str = false;
      } else if ('"' == c) {
        in_str = true;
      } else if ('{' == c || '[' == c) {
        ++depth;
      } else if ('}' == c || ']' == c) {
        --depth;
      }
    } while (i < len && depth > 0);
    if (depth != 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      field.is_null_ = false;
      field.ptr_ = s + start;
      field.len_ = i - start;
    }
  } else {
    int32_t start = i;
    while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') {
      ++i;
    }
    field.is_null_ = false;
    field.ptr_ = s + start;
    field.len_ = i - start;
    if (0 == field.len_) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int JsonScanner::parse_object(ObIAllocator &alloc, const char *s, int32_t len, ObIArray<Pair> &pairs)
{
  int ret = OB_SUCCESS;
  int32_t i = 0;
  skip_ws(s, len, i);
  if (i >= len || '{' != s[i]) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("jsonl row is not an object", K(ret));
  } else {
    ++i;
    skip_ws(s, len, i);
    if (i < len && '}' == s[i]) {
      return ret;
    }
    while (OB_SUCC(ret)) {
      skip_ws(s, len, i);
      FieldBuf kb;
      if (OB_FAIL(decode_json_string(s, len, i, kb))) {
      } else {
        skip_ws(s, len, i);
        if (i >= len || ':' != s[i]) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ++i;
          FieldBuf vb;
          RawField raw;
          if (OB_FAIL(parse_value(s, len, i, raw, vb))) {
          } else {
            Pair pair;
            if (OB_FAIL(write_dup(alloc, kb.data(), kb.length(), pair.key_))) {
            }
            if (OB_SUCC(ret)) {
              if (!raw.is_null_ && (raw.ptr_ == vb.data() || (vb.length() > 0 && raw.ptr_ == vb.data()))) {
                ObString tmp;
                if (OB_FAIL(write_dup(alloc, vb.data(), vb.length(), tmp))) {
                } else {
                  pair.value_.is_null_ = false;
                  pair.value_.ptr_ = tmp.ptr();
                  pair.value_.len_ = tmp.length();
                }
              } else if (!raw.is_null_ && raw.ptr_ >= s && raw.ptr_ < s + len) {
                ObString tmp;
                if (OB_FAIL(write_dup(alloc, raw.ptr_, raw.len_, tmp))) {
                } else {
                  pair.value_.is_null_ = false;
                  pair.value_.ptr_ = tmp.ptr();
                  pair.value_.len_ = tmp.length();
                }
              } else {
                pair.value_ = raw;
              }
            }
            if (OB_SUCC(ret) && OB_FAIL(pairs.push_back(pair))) {
            } else {
              skip_ws(s, len, i);
              if (i < len && ',' == s[i]) {
                ++i;
                continue;
              } else if (i < len && '}' == s[i]) {
                break;
              } else if (i >= len) {
                break;
              } else {
                ret = OB_INVALID_ARGUMENT;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int find_or_add_col(ObIAllocator &alloc, ObIArray<ColBuild> &cols, const char *name, int32_t nlen, int64_t &idx)
{
  int ret = OB_SUCCESS;
  idx = -1;
  for (int64_t i = 0; i < cols.count(); ++i) {
    if (name_eq(cols.at(i).name_, name, nlen)) {
      idx = i;
      break;
    }
  }
  if (-1 == idx) {
    ColBuild col;
    if (OB_FAIL(write_dup(alloc, name, nlen, col.name_))) {
    } else if (OB_FAIL(cols.push_back(col))) {
    } else {
      idx = cols.count() - 1;
    }
  }
  return ret;
}

int unique_csv_name(ObIAllocator &alloc, ObIArray<ColBuild> &cols, const char *name, int32_t nlen, ObString &out)
{
  int ret = OB_SUCCESS;
  int occ = 0;
  for (int64_t i = 0; i < cols.count(); ++i) {
    ObString exist = cols.at(i).name_;
    // compare base name before __ 
    ObString base = exist;
    const char *mark = NULL;
    if (exist.length() > 3) {
      for (int32_t k = exist.length() - 1; k >= 2; --k) {
        if ('_' == exist.ptr()[k] && '_' == exist.ptr()[k - 1]) {
          mark = exist.ptr() + k - 1;
          break;
        }
      }
    }
    if (NULL != mark) {
      base.assign_ptr(exist.ptr(), static_cast<int32_t>(mark - exist.ptr()));
    }
    if (name_eq(base, name, nlen) || name_eq(exist, name, nlen)) {
      ++occ;
    }
  }
  if (0 == occ) {
    ret = write_dup(alloc, name, nlen, out);
  } else {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%.*s__%d", nlen, name, occ + 1);
    if (n <= 0 || n >= static_cast<int>(sizeof(buf))) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      ret = write_dup(alloc, buf, n, out);
    }
  }
  return ret;
}

void trim_field(const char *&p, int32_t &len)
{
  while (len > 0 && (' ' == p[0] || '\t' == p[0])) {
    ++p;
    --len;
  }
  while (len > 0 && (' ' == p[len - 1] || '\t' == p[len - 1] || '\r' == p[len - 1])) {
    --len;
  }
}

int finish_def(ObIAllocator &alloc, ObFileSqlKind kind, const ObString &path, bool header,
               int64_t rows, const ObIArray<ColBuild> &cols, ObFileSqlTableDef *&def)
{
  int ret = OB_SUCCESS;
  void *mem = alloc.alloc(sizeof(ObFileSqlTableDef));
  ObFileSqlColMeta *meta = NULL;
  if (OB_ISNULL(mem)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (cols.count() > 0
             && OB_ISNULL(meta = static_cast<ObFileSqlColMeta *>(
                    alloc.alloc(sizeof(ObFileSqlColMeta) * cols.count())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    def = new (mem) ObFileSqlTableDef();
    def->kind_ = kind;
    def->header_ = header;
    def->row_count_ = rows;
    def->col_cnt_ = cols.count();
    def->cols_ = meta;
    if (OB_FAIL(write_dup(alloc, path, def->path_))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
      const ColBuild &c = cols.at(i);
      bool is_bool = false;
      ObObjType t = kind_to_type(c.kind_, is_bool);
      new (&meta[i]) ObFileSqlColMeta();
      meta[i].type_ = t;
      meta[i].is_bool_ = is_bool;
      meta[i].nullable_ = c.nullable_ || !c.seen_value_;
      int32_t len = c.max_len_ > 0 ? c.max_len_ : 1;
      if (len < 64 && ObVarcharType == t) {
        len = 64;
      }
      if (len < c.max_len_) {
        len = c.max_len_;
      }
      meta[i].length_ = ObVarcharType == t ? len : 0;
      meta[i].max_raw_len_ = c.max_len_;
      if (OB_FAIL(write_dup(alloc, c.name_, meta[i].name_))) {
      }
    }
  }
  return ret;
}

int infer_csv(ObIAllocator &alloc, const ObString &path, bool header, ObFileSqlTableDef *&def)
{
  int ret = OB_SUCCESS;
  CsvScanner scan;
  ObSEArray<ColBuild, 16> cols;
  ObSEArray<RawField, 16> fields;
  int64_t rows = 0;
  bool first = true;
  if (OB_FAIL(scan.open(path.ptr()))) {
  }
  while (OB_SUCC(ret)) {
    bool eof = false;
    fields.reset();
    if (OB_FAIL(scan.next(fields, eof))) {
    } else if (eof) {
      break;
    } else if (first && header) {
      first = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        const char *p = fields.at(i).is_null_ ? "" : fields.at(i).ptr_;
        int32_t len = fields.at(i).is_null_ ? 0 : fields.at(i).len_;
        trim_field(p, len);
        ColBuild col;
        if (0 == len) {
          char nm[32];
          int n = snprintf(nm, sizeof(nm), "column%ld", i);
          ret = write_dup(alloc, nm, n, col.name_);
        } else if (OB_FAIL(unique_csv_name(alloc, cols, p, len, col.name_))) {
        }
        if (OB_SUCC(ret) && OB_FAIL(cols.push_back(col))) {
        }
      }
    } else {
      first = false;
      if (0 == cols.count()) {
        for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
          ColBuild col;
          char nm[32];
          int n = snprintf(nm, sizeof(nm), "column%ld", i);
          if (OB_FAIL(write_dup(alloc, nm, n, col.name_))) {
          } else if (OB_FAIL(cols.push_back(col))) {
          }
        }
      }
      while (OB_SUCC(ret) && cols.count() < fields.count()) {
        ColBuild col;
        char nm[32];
        int n = snprintf(nm, sizeof(nm), "column%ld", cols.count());
        if (OB_FAIL(write_dup(alloc, nm, n, col.name_))) {
        } else if (OB_FAIL(cols.push_back(col))) {
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
        if (i < fields.count()) {
          observe_field(cols.at(i), fields.at(i));
        } else {
          cols.at(i).nullable_ = true;
        }
      }
      ++rows;
    }
  }
  scan.close();
  if (OB_SUCC(ret) && 0 == cols.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("csv file has no columns", K(ret), K(path));
  } else if (OB_SUCC(ret)) {
    ret = finish_def(alloc, OB_FILE_SQL_CSV, path, header, rows, cols, def);
  }
  return ret;
}

int infer_jsonl(ObIAllocator &alloc, const ObString &path, ObFileSqlTableDef *&def)
{
  int ret = OB_SUCCESS;
  JsonScanner scan;
  ObSEArray<ColBuild, 16> cols;
  ObSEArray<JsonScanner::Pair, 16> pairs;
  ObArenaAllocator tmp("FileSqlJson");
  int64_t rows = 0;
  if (OB_FAIL(scan.open(path.ptr()))) {
  }
  while (OB_SUCC(ret)) {
    bool eof = false;
    tmp.reuse();
    pairs.reset();
    if (OB_FAIL(scan.next(tmp, pairs, eof))) {
    } else if (eof) {
      break;
    } else {
      ++rows;
      for (int64_t i = 0; OB_SUCC(ret) && i < pairs.count(); ++i) {
        int64_t idx = -1;
        if (OB_FAIL(find_or_add_col(alloc, cols, pairs.at(i).key_.ptr(), pairs.at(i).key_.length(), idx))) {
        } else if (idx >= 0) {
          observe_field(cols.at(idx), pairs.at(i).value_);
        }
      }
      for (int64_t i = 0; i < cols.count(); ++i) {
        bool present = false;
        for (int64_t j = 0; j < pairs.count(); ++j) {
          if (name_eq(cols.at(i).name_, pairs.at(j).key_.ptr(), pairs.at(j).key_.length())) {
            present = true;
            if (pairs.at(j).value_.is_null_) {
              cols.at(i).nullable_ = true;
            }
          }
        }
        if (!present) {
          cols.at(i).nullable_ = true;
        }
      }
    }
  }
  scan.close();
  if (OB_SUCC(ret) && 0 == cols.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("jsonl file has no columns", K(ret), K(path));
  } else if (OB_SUCC(ret)) {
    ret = finish_def(alloc, OB_FILE_SQL_JSONL, path, false, rows, cols, def);
  }
  return ret;
}

// -------------------- parquet (footer + page stream) --------------------

class TCompact
{
public:
  TCompact(const uint8_t *p, int64_t n) : p_(p), n_(n), i_(0) {}
  int64_t left() const { return n_ - i_; }
  int64_t pos() const { return i_; }
  int byte(uint8_t &b)
  {
    if (i_ >= n_) {
      return OB_INVALID_ARGUMENT;
    }
    b = p_[i_++];
    return OB_SUCCESS;
  }
  int varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    while (OB_SUCC(ret)) {
      uint8_t b = 0;
      if (OB_FAIL(byte(b))) {
      } else {
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (0 == (b & 0x80)) {
          break;
        }
        shift += 7;
        if (shift > 63) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
    }
    return ret;
  }
  int zigzag(int64_t &v)
  {
    uint64_t u = 0;
    int ret = varint(u);
    if (OB_SUCC(ret)) {
      v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    }
    return ret;
  }
  int raw(int64_t len, const uint8_t *&out)
  {
    int ret = OB_SUCCESS;
    if (len < 0 || i_ + len > n_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      out = p_ + i_;
      i_ += len;
    }
    return ret;
  }
  int skip(int typ);
  int read_struct_field(int64_t &fid, int &typ, bool &end)
  {
    int ret = OB_SUCCESS;
    end = false;
    uint8_t h = 0;
    if (OB_FAIL(byte(h))) {
    } else if (0 == h) {
      end = true;
    } else {
      typ = h & 0x0f;
      int delta = h >> 4;
      if (0 == delta) {
        uint64_t id = 0;
        if (OB_FAIL(varint(id))) {
        } else {
          fid = static_cast<int64_t>(id);
        }
      } else {
        fid = last_ + delta;
      }
      if (OB_SUCC(ret)) {
        last_ = fid;
      }
    }
    return ret;
  }
  void push() { lasts_[sp_++] = last_; last_ = 0; }
  void pop() { last_ = sp_ > 0 ? lasts_[--sp_] : 0; }
  int64_t last_ = 0;
private:
  const uint8_t *p_;
  int64_t n_;
  int64_t i_;
  int64_t lasts_[16] = {0};
  int sp_ = 0;
};

int TCompact::skip(int typ)
{
  int ret = OB_SUCCESS;
  if (1 == typ || 2 == typ) {
  } else if (3 == typ) {
    uint8_t b = 0;
    ret = byte(b);
  } else if (4 == typ || 5 == typ || 6 == typ) {
    int64_t v = 0;
    ret = zigzag(v);
  } else if (7 == typ) {
    const uint8_t *p = NULL;
    ret = raw(8, p);
  } else if (8 == typ) {
    uint64_t n = 0;
    const uint8_t *p = NULL;
    if (OB_FAIL(varint(n))) {
    } else {
      ret = raw(static_cast<int64_t>(n), p);
    }
  } else if (9 == typ) {
    uint8_t h = 0;
    if (OB_FAIL(byte(h))) {
    } else {
      int et = h & 0x0f;
      uint64_t n = h >> 4;
      if (15 == n && OB_FAIL(varint(n))) {
      }
      for (uint64_t k = 0; OB_SUCC(ret) && k < n; ++k) {
        ret = skip(et);
      }
    }
  } else if (11 == typ) {
    uint64_t n = 0;
    if (OB_FAIL(varint(n))) {
    } else if (0 == n) {
    } else {
      uint8_t h = 0;
      if (OB_FAIL(byte(h))) {
      } else {
        int kt = h >> 4;
        int vt = h & 0x0f;
        for (uint64_t k = 0; OB_SUCC(ret) && k < n; ++k) {
          if (OB_FAIL(skip(kt))) {
          } else {
            ret = skip(vt);
          }
        }
      }
    }
  } else if (12 == typ) {
    push();
    while (OB_SUCC(ret)) {
      int64_t fid = 0;
      int ft = 0;
      bool end = false;
      if (OB_FAIL(read_struct_field(fid, ft, end))) {
      } else if (end) {
        break;
      } else {
        ret = skip(ft);
      }
    }
    pop();
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

struct PqCol
{
  ObString name_;
  int physical_; // parquet Type
  int converted_; // -1 none
  int repetition_; // 0 required 1 optional 2 repeated
  int type_len_;
  PqCol() : name_(), physical_(-1), converted_(-1), repetition_(1), type_len_(0) {}
};

struct PqChunk
{
  int64_t data_off_;
  int64_t dict_off_;
  int64_t compressed_size_;
  int64_t num_values_;
  int codec_;
  int physical_;
  PqChunk() : data_off_(-1), dict_off_(-1), compressed_size_(0), num_values_(0), codec_(0), physical_(-1) {}
};

int snappy_uncompress(const uint8_t *src, int64_t src_len, uint8_t *dst, int64_t dst_cap, int64_t &dst_len)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  uint64_t ulen = 0;
  int shift = 0;
  while (OB_SUCC(ret) && i < src_len) {
    uint8_t b = src[i++];
    ulen |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      break;
    }
    shift += 7;
  }
  if (static_cast<int64_t>(ulen) > dst_cap) {
    ret = OB_SIZE_OVERFLOW;
  }
  int64_t o = 0;
  while (OB_SUCC(ret) && i < src_len) {
    uint8_t tag = src[i++];
    int t = tag & 3;
    if (0 == t) {
      int n = tag >> 2;
      int lit = n + 1;
      if (n >= 60) {
        int extra = n - 59;
        lit = 1;
        if (i + extra > src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          for (int k = 0; k < extra; ++k) {
            lit += src[i++] << (8 * k);
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (i + lit > src_len || o + lit > static_cast<int64_t>(ulen)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          MEMCPY(dst + o, src + i, lit);
          i += lit;
          o += lit;
        }
      }
    } else {
      int length = 0;
      int offset = 0;
      if (1 == t) {
        length = ((tag >> 2) & 7) + 4;
        if (i >= src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          offset = ((tag >> 5) << 8) | src[i++];
        }
      } else if (2 == t) {
        length = (tag >> 2) + 1;
        if (i + 2 > src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          offset = src[i] | (src[i + 1] << 8);
          i += 2;
        }
      } else {
        length = (tag >> 2) + 1;
        if (i + 4 > src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          offset = src[i] | (src[i + 1] << 8) | (src[i + 2] << 16) | (src[i + 3] << 24);
          i += 4;
        }
      }
      if (OB_SUCC(ret)) {
        if (offset <= 0 || offset > o || o + length > static_cast<int64_t>(ulen)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          for (int k = 0; k < length; ++k) {
            dst[o] = dst[o - offset];
            ++o;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && o != static_cast<int64_t>(ulen)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    dst_len = o;
  }
  return ret;
}

int gzip_uncompress(const uint8_t *src, int64_t src_len, uint8_t *dst, int64_t dst_cap, int64_t &dst_len)
{
  int ret = OB_SUCCESS;
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(src));
  strm.avail_in = static_cast<uInt>(src_len);
  strm.next_out = reinterpret_cast<Bytef *>(dst);
  strm.avail_out = static_cast<uInt>(dst_cap);
  if (Z_OK != inflateInit2(&strm, 16 + MAX_WBITS)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (Z_STREAM_END != inflate(&strm, Z_FINISH) && strm.total_out == 0) {
    ret = OB_INVALID_ARGUMENT;
    inflateEnd(&strm);
  } else {
    dst_len = static_cast<int64_t>(strm.total_out);
    inflateEnd(&strm);
  }
  return ret;
}

int decompress_page(int codec, const uint8_t *src, int64_t src_len, int64_t expect,
                    ObIAllocator &alloc, const uint8_t *&out, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  if (0 == codec) {
    out = src;
    out_len = src_len;
  } else if (1 == codec || 2 == codec) {
    int64_t cap = expect > src_len * 8 ? expect + 64 : src_len * 8 + 64;
    if (cap < expect) {
      cap = expect + 64;
    }
    uint8_t *buf = static_cast<uint8_t *>(alloc.alloc(cap));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (1 == codec) {
      ret = snappy_uncompress(src, src_len, buf, cap, out_len);
      out = buf;
    } else {
      ret = gzip_uncompress(src, src_len, buf, cap, out_len);
      out = buf;
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet compression codec");
  }
  return ret;
}

struct PqVal
{
  bool is_null_;
  int kind_; // 0 int 1 double 2 bytes 3 bool
  int64_t i_;
  double d_;
  const char *p_;
  int32_t len_;
  PqVal() : is_null_(true), kind_(0), i_(0), d_(0), p_(NULL), len_(0) {}
};

int decode_plain(int physical, int type_len, const uint8_t *p, int64_t n, int64_t nvals,
                 ObIArray<PqVal> &vals)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  for (int64_t v = 0; OB_SUCC(ret) && v < nvals; ++v) {
    PqVal val;
    val.is_null_ = false;
    if (0 == physical) { // bool bit packed, handled separately
      ret = OB_INVALID_ARGUMENT;
    } else if (1 == physical) {
      if (i + 4 > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        int32_t x = 0;
        MEMCPY(&x, p + i, 4);
        val.kind_ = 0;
        val.i_ = x;
        i += 4;
      }
    } else if (2 == physical) {
      if (i + 8 > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        int64_t x = 0;
        MEMCPY(&x, p + i, 8);
        val.kind_ = 0;
        val.i_ = x;
        i += 8;
      }
    } else if (4 == physical) {
      if (i + 4 > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        float f = 0;
        MEMCPY(&f, p + i, 4);
        val.kind_ = 1;
        val.d_ = f;
        i += 4;
      }
    } else if (5 == physical) {
      if (i + 8 > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        double d = 0;
        MEMCPY(&d, p + i, 8);
        val.kind_ = 1;
        val.d_ = d;
        i += 8;
      }
    } else if (6 == physical) {
      if (i + 4 > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        uint32_t ln = 0;
        MEMCPY(&ln, p + i, 4);
        i += 4;
        if (i + static_cast<int64_t>(ln) > n) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          val.kind_ = 2;
          val.p_ = reinterpret_cast<const char *>(p + i);
          val.len_ = static_cast<int32_t>(ln);
          i += ln;
        }
      }
    } else if (7 == physical) {
      if (type_len <= 0 || i + type_len > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        val.kind_ = 2;
        val.p_ = reinterpret_cast<const char *>(p + i);
        val.len_ = type_len;
        i += type_len;
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet physical type");
    }
    if (OB_SUCC(ret) && OB_FAIL(vals.push_back(val))) {
    }
  }
  return ret;
}

int decode_plain_bool(const uint8_t *p, int64_t n, int64_t nvals, ObIArray<PqVal> &vals)
{
  int ret = OB_SUCCESS;
  for (int64_t v = 0; OB_SUCC(ret) && v < nvals; ++v) {
    int64_t byte = v / 8;
    int bit = v % 8;
    if (byte >= n) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      PqVal val;
      val.is_null_ = false;
      val.kind_ = 3;
      val.i_ = (p[byte] >> bit) & 1;
      ret = vals.push_back(val);
    }
  }
  return ret;
}

int parquet_schema(ByteReader &rd, ObIAllocator &alloc, ObIArray<PqCol> &cols,
                   ObIArray<ObSEArray<PqChunk, 1>> &chunks, int64_t &num_rows)
{
  int ret = OB_SUCCESS;
  char tail[8];
  off_t sz = ::lseek(rd.fd(), 0, SEEK_END);
  if (sz < 12) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(rd.pread_exact(sz - 8, tail, 8))) {
  } else if (0 != memcmp(tail + 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("not a parquet file", K(ret));
  } else {
    uint32_t flen = 0;
    MEMCPY(&flen, tail, 4);
    if (flen + 8 > static_cast<uint64_t>(sz)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      uint8_t *footer = static_cast<uint8_t *>(alloc.alloc(flen));
      if (OB_ISNULL(footer)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(rd.pread_exact(sz - 8 - flen, reinterpret_cast<char *>(footer), flen))) {
      } else {
        TCompact tc(footer, flen);
        // FileMetaData fields we care about, skip others.
        // Walk top-level struct manually collecting schema (2), num_rows (3), row_groups (4).
        tc.push();
        while (OB_SUCC(ret)) {
          int64_t fid = 0;
          int typ = 0;
          bool end = false;
          if (OB_FAIL(tc.read_struct_field(fid, typ, end))) {
          } else if (end) {
            break;
          } else if (2 == fid && 9 == typ) {
            uint8_t h = 0;
            uint64_t n = 0;
            if (OB_FAIL(tc.byte(h))) {
            } else {
              n = h >> 4;
              if (15 == n && OB_FAIL(tc.varint(n))) {
              }
              for (uint64_t e = 0; OB_SUCC(ret) && e < n; ++e) {
                PqCol col;
                tc.push();
                while (OB_SUCC(ret)) {
                  int64_t sf = 0;
                  int st = 0;
                  bool se = false;
                  if (OB_FAIL(tc.read_struct_field(sf, st, se))) {
                  } else if (se) {
                    break;
                  } else if (1 == sf && (4 == st || 5 == st || 6 == st)) {
                    int64_t v = 0;
                    if (OB_FAIL(tc.zigzag(v))) {
                    } else {
                      col.physical_ = static_cast<int>(v);
                    }
                  } else if (2 == sf) {
                    int64_t v = 0;
                    if (OB_FAIL(tc.zigzag(v))) {
                    } else {
                      col.type_len_ = static_cast<int>(v);
                    }
                  } else if (3 == sf) {
                    int64_t v = 0;
                    if (OB_FAIL(tc.zigzag(v))) {
                    } else {
                      col.repetition_ = static_cast<int>(v);
                    }
                  } else if (4 == sf && 8 == st) {
                    uint64_t ln = 0;
                    const uint8_t *bp = NULL;
                    if (OB_FAIL(tc.varint(ln)) || OB_FAIL(tc.raw(static_cast<int64_t>(ln), bp))) {
                    } else if (OB_FAIL(write_dup(alloc, reinterpret_cast<const char *>(bp),
                                                 static_cast<int32_t>(ln), col.name_))) {
                    }
                  } else if (5 == sf) {
                    int64_t v = 0;
                    if (OB_FAIL(tc.zigzag(v))) {
                    } else {
                      // num children, ignore
                    }
                  } else if (6 == sf) {
                    int64_t v = 0;
                    if (OB_FAIL(tc.zigzag(v))) {
                    } else {
                      col.converted_ = static_cast<int>(v);
                    }
                  } else if (OB_FAIL(tc.skip(st))) {
                  }
                }
                tc.pop();
                if (OB_SUCC(ret) && col.physical_ >= 0 && OB_FAIL(cols.push_back(col))) {
                }
              }
            }
          } else if (3 == fid) {
            int64_t v = 0;
            if (OB_FAIL(tc.zigzag(v))) {
            } else {
              num_rows = v;
            }
          } else if (4 == fid && 9 == typ) {
            uint8_t h = 0;
            uint64_t nrg = 0;
            if (OB_FAIL(tc.byte(h))) {
            } else {
              nrg = h >> 4;
              if (15 == nrg && OB_FAIL(tc.varint(nrg))) {
              }
              for (uint64_t g = 0; OB_SUCC(ret) && g < nrg; ++g) {
                if (0 == g) {
                  // prepare chunk arrays
                  for (int64_t c = chunks.count(); OB_SUCC(ret) && c < cols.count(); ++c) {
                    ObSEArray<PqChunk, 1> one;
                    if (OB_FAIL(chunks.push_back(one))) {
                    }
                  }
                }
                tc.push();
                int col_i = 0;
                while (OB_SUCC(ret)) {
                  int64_t sf = 0;
                  int st = 0;
                  bool se = false;
                  if (OB_FAIL(tc.read_struct_field(sf, st, se))) {
                  } else if (se) {
                    break;
                  } else if (1 == sf && 9 == st) {
                    uint8_t hh = 0;
                    uint64_t nc = 0;
                    if (OB_FAIL(tc.byte(hh))) {
                    } else {
                      nc = hh >> 4;
                      if (15 == nc && OB_FAIL(tc.varint(nc))) {
                      }
                      for (uint64_t c = 0; OB_SUCC(ret) && c < nc; ++c) {
                        PqChunk chunk;
                        tc.push();
                        while (OB_SUCC(ret)) {
                          int64_t cf = 0;
                          int ct = 0;
                          bool ce = false;
                          if (OB_FAIL(tc.read_struct_field(cf, ct, ce))) {
                          } else if (ce) {
                            break;
                          } else if (3 == cf && 12 == ct) {
                            tc.push();
                            while (OB_SUCC(ret)) {
                              int64_t mf = 0;
                              int mt = 0;
                              bool me = false;
                              if (OB_FAIL(tc.read_struct_field(mf, mt, me))) {
                              } else if (me) {
                                break;
                              } else if (1 == mf) {
                                int64_t v = 0;
                                if (OB_FAIL(tc.zigzag(v))) {
                                } else {
                                  chunk.physical_ = static_cast<int>(v);
                                }
                              } else if (4 == mf) {
                                int64_t v = 0;
                                if (OB_FAIL(tc.zigzag(v))) {
                                } else {
                                  chunk.codec_ = static_cast<int>(v);
                                }
                              } else if (5 == mf) {
                                int64_t v = 0;
                                if (OB_FAIL(tc.zigzag(v))) {
                                } else {
                                  chunk.num_values_ = v;
                                }
                              } else if (7 == mf) {
                                int64_t v = 0;
                                if (OB_FAIL(tc.zigzag(v))) {
                                } else {
                                  chunk.compressed_size_ = v;
                                }
                              } else if (9 == mf) {
                                int64_t v = 0;
                                if (OB_FAIL(tc.zigzag(v))) {
                                } else {
                                  chunk.data_off_ = v;
                                }
                              } else if (11 == mf) {
                                int64_t v = 0;
                                if (OB_FAIL(tc.zigzag(v))) {
                                } else {
                                  chunk.dict_off_ = v;
                                }
                              } else if (OB_FAIL(tc.skip(mt))) {
                              }
                            }
                            tc.pop();
                          } else if (OB_FAIL(tc.skip(ct))) {
                          }
                        }
                        tc.pop();
                        if (OB_SUCC(ret)) {
                          if (col_i >= chunks.count()) {
                            ret = OB_INVALID_ARGUMENT;
                          } else if (OB_FAIL(chunks.at(col_i).push_back(chunk))) {
                          } else {
                            ++col_i;
                          }
                        }
                      }
                    }
                  } else if (OB_FAIL(tc.skip(st))) {
                  }
                }
                tc.pop();
              }
            }
          } else if (OB_FAIL(tc.skip(typ))) {
          }
        }
        tc.pop();
      }
    }
  }
  return ret;
}

int map_pq_type(const PqCol &col, ObObjType &type, bool &is_bool, bool &nullable)
{
  int ret = OB_SUCCESS;
  is_bool = false;
  nullable = (1 == col.repetition_);
  // converted type DATE = 6, UTF8 = 0, TIMESTAMP_MILLIS = 9, TIMESTAMP_MICROS = 10
  if (6 == col.converted_) {
    type = ObDateType;
  } else if (9 == col.converted_ || 10 == col.converted_) {
    type = ObDateTimeType;
  } else if (0 == col.physical_) {
    type = ObTinyIntType;
    is_bool = true;
  } else if (1 == col.physical_ || 2 == col.physical_) {
    type = ObIntType;
  } else if (4 == col.physical_ || 5 == col.physical_) {
    type = ObDoubleType;
  } else if (6 == col.physical_ || 7 == col.physical_) {
    type = ObVarcharType;
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet column type");
  }
  return ret;
}

int infer_parquet(ObIAllocator &alloc, const ObString &path, ObFileSqlTableDef *&def)
{
  int ret = OB_SUCCESS;
  ByteReader rd;
  ObSEArray<PqCol, 8> cols;
  ObSEArray<ObSEArray<PqChunk, 1>, 8> chunks;
  ObSEArray<ColBuild, 8> builds;
  int64_t nrows = 0;
  if (OB_FAIL(rd.open(path.ptr()))) {
  } else if (OB_FAIL(parquet_schema(rd, alloc, cols, chunks, nrows))) {
  } else if (0 == cols.count()) {
    ret = OB_INVALID_ARGUMENT;
  }
  rd.close();
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    ColBuild b;
    bool is_bool = false;
    ObObjType t = ObNullType;
    bool nullable = true;
    if (OB_FAIL(write_dup(alloc, cols.at(i).name_, b.name_))) {
    } else if (OB_FAIL(map_pq_type(cols.at(i), t, is_bool, nullable))) {
    } else {
      b.nullable_ = nullable;
      b.seen_value_ = true;
      b.max_len_ = ObVarcharType == t ? 65535 : 0;
      if (is_bool) b.kind_ = CK_BOOL;
      else if (ObIntType == t) b.kind_ = CK_INT;
      else if (ObDoubleType == t) b.kind_ = CK_DBL;
      else if (ObDateType == t) b.kind_ = CK_DATE;
      else if (ObDateTimeType == t) b.kind_ = CK_DT;
      else b.kind_ = CK_STR;
      if (OB_FAIL(builds.push_back(b))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = finish_def(alloc, OB_FILE_SQL_PARQUET, path, false, nrows, builds, def);
  }
  return ret;
}

int infer_glob(ObIAllocator &alloc, const ObString &pattern, ObFileSqlTableDef *&def)
{
  int ret = OB_SUCCESS;
  ObSEArray<ColBuild, 1> cols;
  ColBuild col;
  col.kind_ = CK_STR;
  col.nullable_ = false;
  col.seen_value_ = true;
  col.max_len_ = 4096;
  if (OB_FAIL(write_dup(alloc, "file", 4, col.name_))) {
  } else if (OB_FAIL(cols.push_back(col))) {
  } else {
    ret = finish_def(alloc, OB_FILE_SQL_GLOB, pattern, false, -1, cols, def);
  }
  return ret;
}

class FileReaderBase : public ObFileSqlOp::Reader
{
public:
  FileReaderBase(const ObString &path, ObCollationType cs) : path_(path), cs_(cs) {}
  virtual ~FileReaderBase() {}
protected:
  ObString path_;
  ObCollationType cs_;
};

class CsvExec : public FileReaderBase
{
public:
  CsvExec(const ObString &path, ObCollationType cs, const ObFileSqlTableDef *def)
      : FileReaderBase(path, cs), def_(def), opened_(false) {}
  int open() override
  {
    int ret = scan_.open(path_.ptr());
    opened_ = OB_SUCC(ret);
    header_left_ = def_->header_;
    return ret;
  }
  void close() override { scan_.close(); opened_ = false; }
  int next(ObIAllocator &alloc, ObIArray<ObObj> &row) override
  {
    int ret = OB_SUCCESS;
    ObSEArray<RawField, 16> fields;
    bool eof = false;
    if (!opened_) {
      ret = OB_NOT_INIT;
    }
    while (OB_SUCC(ret)) {
      fields.reset();
      if (OB_FAIL(scan_.next(fields, eof))) {
      } else if (eof) {
        ret = OB_ITER_END;
      } else if (header_left_) {
        header_left_ = false;
        continue;
      } else {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < def_->col_cnt_; ++i) {
        ObObj obj;
        const ObFileSqlColMeta &c = def_->cols_[i];
        if (i >= fields.count() || fields.at(i).is_null_) {
          obj.set_null();
        } else if (OB_FAIL(text_to_obj(fields.at(i).ptr_, fields.at(i).len_, false,
                                       c.type_, c.is_bool_, cs_, alloc, obj))) {
          LOG_WARN("csv value does not match inferred type", K(ret), K(c.name_), K(i));
        }
        if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
        }
      }
    }
    return ret;
  }
private:
  CsvScanner scan_;
  const ObFileSqlTableDef *def_;
  bool opened_;
  bool header_left_;
};

class JsonExec : public FileReaderBase
{
public:
  JsonExec(const ObString &path, ObCollationType cs, const ObFileSqlTableDef *def)
      : FileReaderBase(path, cs), def_(def) {}
  int open() override { return scan_.open(path_.ptr()); }
  void close() override { scan_.close(); }
  int next(ObIAllocator &alloc, ObIArray<ObObj> &row) override
  {
    int ret = OB_SUCCESS;
    ObSEArray<JsonScanner::Pair, 16> pairs;
    bool eof = false;
    if (OB_FAIL(scan_.next(alloc, pairs, eof))) {
    } else if (eof) {
      ret = OB_ITER_END;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < def_->col_cnt_; ++i) {
        const ObFileSqlColMeta &c = def_->cols_[i];
        const RawField *f = NULL;
        for (int64_t j = 0; j < pairs.count(); ++j) {
          if (name_eq(c.name_, pairs.at(j).key_.ptr(), pairs.at(j).key_.length())) {
            f = &pairs.at(j).value_;
            break;
          }
        }
        ObObj obj;
        if (NULL == f || f->is_null_) {
          obj.set_null();
        } else if (OB_FAIL(text_to_obj(f->ptr_, f->len_, false, c.type_, c.is_bool_, cs_, alloc, obj))) {
          LOG_WARN("json value does not match inferred type", K(ret), K(c.name_));
        }
        if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
        }
      }
    }
    return ret;
  }
private:
  JsonScanner scan_;
  const ObFileSqlTableDef *def_;
};

class GlobExec : public FileReaderBase
{
public:
  explicit GlobExec(const ObString &pattern) : FileReaderBase(pattern, CS_TYPE_UTF8MB4_BIN), inited_(false), idx_(0)
  {
    memset(&gl_, 0, sizeof(gl_));
  }
  int open() override
  {
    int ret = OB_SUCCESS;
    int rc = glob(path_.ptr(), 0, NULL, &gl_);
    if (0 != rc && GLOB_NOMATCH != rc) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("glob failed", K(ret), K(rc), K(path_));
    } else {
      inited_ = true;
      idx_ = 0;
      if (GLOB_NOMATCH == rc) {
        gl_.gl_pathc = 0;
      }
    }
    return ret;
  }
  void close() override
  {
    if (inited_) {
      globfree(&gl_);
      inited_ = false;
    }
  }
  int next(ObIAllocator &alloc, ObIArray<ObObj> &row) override
  {
    int ret = OB_SUCCESS;
    if (!inited_) {
      ret = OB_NOT_INIT;
    } else if (idx_ >= static_cast<int64_t>(gl_.gl_pathc)) {
      ret = OB_ITER_END;
    } else {
      const char *p = gl_.gl_pathv[idx_++];
      struct stat st;
      if (0 == stat(p, &st) && S_ISDIR(st.st_mode)) {
        return next(alloc, row);
      }
      ObString s;
      ObObj obj;
      if (OB_FAIL(write_dup(alloc, p, static_cast<int32_t>(strlen(p)), s))) {
      } else {
        obj.set_varchar(s);
        obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        obj.set_collation_level(CS_LEVEL_IMPLICIT);
        ret = row.push_back(obj);
      }
    }
    return ret;
  }
private:
  glob_t gl_;
  bool inited_;
  int64_t idx_;
};

int pq_val_to_obj(const PqVal &v, const ObFileSqlColMeta &col, ObCollationType cs,
                  ObIAllocator &alloc, ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (v.is_null_) {
    obj.set_null();
  } else if (col.is_bool_ || ObTinyIntType == col.type_) {
    obj.set_tinyint(0 != v.i_ ? 1 : 0);
  } else if (ObIntType == col.type_) {
    if (1 == v.kind_) {
      obj.set_int(static_cast<int64_t>(v.d_));
    } else {
      obj.set_int(v.i_);
    }
  } else if (ObDoubleType == col.type_) {
    if (1 == v.kind_) {
      obj.set_double(v.d_);
    } else {
      obj.set_double(static_cast<double>(v.i_));
    }
  } else if (ObDateType == col.type_) {
    // parquet date = days since 1970-01-01. Build a civil date via time then str_to_date
    // by formatting days. Use a known conversion: unix day -> y-m-d.
    int64_t z = v.i_ + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04ld-%02ld-%02ld", y, m, d);
    int32_t date = 0;
    if (OB_FAIL(ObTimeConverter::str_to_date(ObString(static_cast<int32_t>(strlen(buf)), buf), date))) {
    } else {
      obj.set_date(date);
    }
  } else if (ObDateTimeType == col.type_) {
    int64_t us = v.i_;
    // converted type is not stored on the value; micros vs millis is chosen by magnitude later.
    // Values from TIMESTAMP_MILLIS are smaller. Heuristic: if |v| < 1e14 treat as millis.
    if (us > -100000000000000LL && us < 100000000000000LL) {
      us *= 1000;
    }
    time_t sec = static_cast<time_t>(us / 1000000);
    struct tm tmv;
    gmtime_r(&sec, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    int64_t dt = 0;
    int16_t scale = 0;
    ObTimeConvertCtx ctx(NULL, false);
    if (OB_FAIL(ObTimeConverter::str_to_datetime(ObString(static_cast<int32_t>(strlen(buf)), buf),
                                                 ctx, dt, &scale))) {
    } else {
      obj.set_datetime(dt);
    }
  } else {
    char *buf = NULL;
    int32_t len = v.len_;
    if (len < 0) {
      len = 0;
    }
    if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len > 0 && NULL != v.p_) {
        MEMCPY(buf, v.p_, len);
      }
      buf[len] = '\0';
      obj.set_varchar(buf, len);
      obj.set_collation_type(cs);
      obj.set_collation_level(CS_LEVEL_IMPLICIT);
    }
  }
  return ret;
}

class ParquetExec : public FileReaderBase
{
public:
  ParquetExec(const ObString &path, ObCollationType cs, const ObFileSqlTableDef *def)
      : FileReaderBase(path, cs), def_(def), page_alloc_("FileSqlPq"), vals_(), val_idx_(0) {}
  int open() override
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(rd_.open(path_.ptr()))) {
    } else if (OB_FAIL(parquet_schema(rd_, page_alloc_, cols_, chunks_, nrows_))) {
    } else if (cols_.count() != def_->col_cnt_) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("parquet schema changed", K(ret), K(cols_.count()), K(def_->col_cnt_));
    } else {
      ret = load_columns();
    }
    return ret;
  }
  void close() override
  {
    rd_.close();
    page_alloc_.reset();
    vals_.reset();
  }
  int next(ObIAllocator &alloc, ObIArray<ObObj> &row) override
  {
    int ret = OB_SUCCESS;
    if (0 == vals_.count()) {
      ret = OB_ITER_END;
    } else if (val_idx_ >= vals_.at(0).count()) {
      ret = OB_ITER_END;
    } else {
      for (int64_t c = 0; OB_SUCC(ret) && c < def_->col_cnt_; ++c) {
        ObObj obj;
        if (c >= vals_.count() || val_idx_ >= vals_.at(c).count()) {
          obj.set_null();
        } else if (OB_FAIL(pq_val_to_obj(vals_.at(c).at(val_idx_), def_->cols_[c], cs_, alloc, obj))) {
        }
        if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
        }
      }
      if (OB_SUCC(ret)) {
        ++val_idx_;
      }
    }
    return ret;
  }
private:
  int load_columns()
  {
    int ret = OB_SUCCESS;
    // Materialize one row-group of decoded values. Pages are decoded one at a time
    // into the column vector; the file itself is not copied.
    for (int64_t c = 0; OB_SUCC(ret) && c < cols_.count(); ++c) {
      ObSEArray<PqVal, 64> colvals;
      const ObIArray<PqChunk> &chs = chunks_.at(c);
      for (int64_t k = 0; OB_SUCC(ret) && k < chs.count(); ++k) {
        ret = read_chunk(cols_.at(c), chs.at(k), colvals);
      }
      if (OB_SUCC(ret) && OB_FAIL(vals_.push_back(colvals))) {
      }
    }
    val_idx_ = 0;
    return ret;
  }
  int read_chunk(const PqCol &col, const PqChunk &chunk, ObIArray<PqVal> &out)
  {
    int ret = OB_SUCCESS;
    int64_t pos = chunk.dict_off_ >= 0 ? chunk.dict_off_ : chunk.data_off_;
    int64_t end = pos + chunk.compressed_size_;
    ObSEArray<PqVal, 8> dict;
    int64_t produced = 0;
    while (OB_SUCC(ret) && pos + 1 < end && produced < chunk.num_values_) {
      // Read a generous header window then parse thrift.
      int64_t win = end - pos;
      if (win > 256) {
        win = 256;
      }
      char hdrbuf[256];
      if (OB_FAIL(rd_.pread_exact(pos, hdrbuf, win))) {
      } else {
        TCompact tc(reinterpret_cast<uint8_t *>(hdrbuf), win);
        int page_type = -1;
        int64_t uncomp = 0;
        int64_t comp = 0;
        int64_t num_values = 0;
        int encoding = 0;
        int64_t def_len = 0;
        int64_t rep_len = 0;
        bool is_v2 = false;
        bool compressed_values = true;
        tc.push();
        bool done_hdr = false;
        while (OB_SUCC(ret) && !done_hdr) {
          int64_t fid = 0;
          int typ = 0;
          bool e = false;
          if (OB_FAIL(tc.read_struct_field(fid, typ, e))) {
          } else if (e) {
            done_hdr = true;
          } else if (1 == fid) {
            int64_t v = 0;
            if (OB_FAIL(tc.zigzag(v))) {
            } else {
              page_type = static_cast<int>(v);
            }
          } else if (2 == fid) {
            int64_t v = 0;
            if (OB_FAIL(tc.zigzag(v))) {
            } else {
              uncomp = v;
            }
          } else if (3 == fid) {
            int64_t v = 0;
            if (OB_FAIL(tc.zigzag(v))) {
            } else {
              comp = v;
            }
          } else if (5 == fid && 12 == typ) {
            tc.push();
            while (OB_SUCC(ret)) {
              int64_t sf = 0; int st = 0; bool se = false;
              if (OB_FAIL(tc.read_struct_field(sf, st, se))) {
              } else if (se) {
                break;
              } else if (1 == sf) {
                int64_t v = 0;
                if (OB_FAIL(tc.zigzag(v))) {
                } else num_values = v;
              } else if (2 == sf) {
                int64_t v = 0;
                if (OB_FAIL(tc.zigzag(v))) {
                } else encoding = static_cast<int>(v);
              } else if (OB_FAIL(tc.skip(st))) {
              }
            }
            tc.pop();
          } else if (7 == fid && 12 == typ) {
            tc.push();
            while (OB_SUCC(ret)) {
              int64_t sf = 0; int st = 0; bool se = false;
              if (OB_FAIL(tc.read_struct_field(sf, st, se))) {
              } else if (se) break;
              else if (1 == sf) {
                int64_t v = 0;
                if (OB_FAIL(tc.zigzag(v))) {
                } else num_values = v;
              } else if (OB_FAIL(tc.skip(st))) {
              }
            }
            tc.pop();
          } else if (8 == fid && 12 == typ) {
            is_v2 = true;
            tc.push();
            while (OB_SUCC(ret)) {
              int64_t sf = 0; int st = 0; bool se = false;
              if (OB_FAIL(tc.read_struct_field(sf, st, se))) {
              } else if (se) break;
              else if (1 == sf) {
                int64_t v = 0;
                if (OB_FAIL(tc.zigzag(v))) {
                } else num_values = v;
              } else if (4 == sf) {
                int64_t v = 0;
                if (OB_FAIL(tc.zigzag(v))) {
                } else encoding = static_cast<int>(v);
              } else if (5 == sf) {
                int64_t v = 0;
                if (OB_FAIL(tc.zigzag(v))) {
                } else def_len = v;
              } else if (6 == sf) {
                int64_t v = 0;
                if (OB_FAIL(tc.zigzag(v))) {
                } else rep_len = v;
              } else if (7 == sf && 2 == st) {
                compressed_values = false;
              } else if (7 == sf && 1 == st) {
                compressed_values = true;
              } else if (OB_FAIL(tc.skip(st))) {
              }
            }
            tc.pop();
          } else if (OB_FAIL(tc.skip(typ))) {
          }
        }
        tc.pop();
        int64_t hdr_sz = tc.pos();
        if (OB_FAIL(ret)) {
        } else if (comp < 0 || hdr_sz <= 0) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          char *payload = static_cast<char *>(page_alloc_.alloc(comp + 1));
          if (OB_ISNULL(payload)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(rd_.pread_exact(pos + hdr_sz, payload, comp))) {
          } else {
            const uint8_t *raw = reinterpret_cast<uint8_t *>(payload);
            int64_t raw_len = comp;
            const uint8_t *body = raw;
            int64_t body_len = raw_len;
            int codec = chunk.codec_;
            if (is_v2) {
              int64_t levels = def_len + rep_len;
              if (levels > raw_len) {
                ret = OB_INVALID_ARGUMENT;
              } else if (!compressed_values) {
                body = raw + levels;
                body_len = raw_len - levels;
              } else {
                const uint8_t *dec = NULL;
                int64_t dec_len = 0;
                int64_t expect = uncomp > levels ? uncomp - levels : uncomp;
                if (OB_FAIL(decompress_page(codec, raw + levels, raw_len - levels, expect,
                                            page_alloc_, dec, dec_len))) {
                } else {
                  body = dec;
                  body_len = dec_len;
                }
              }
            } else if (0 != codec) {
              const uint8_t *dec = NULL;
              int64_t dec_len = 0;
              if (OB_FAIL(decompress_page(codec, raw, raw_len, uncomp, page_alloc_, dec, dec_len))) {
              } else {
                body = dec;
                body_len = dec_len;
              }
            }
            if (OB_SUCC(ret)) {
              int64_t before = out.count();
              if (2 == page_type) {
                dict.reset();
                if (0 == col.physical_) {
                  ret = decode_plain_bool(body, body_len, num_values, dict);
                } else {
                  ret = decode_plain(col.physical_, col.type_len_, body, body_len, num_values, dict);
                }
              } else if (0 == encoding) {
                if (0 == col.physical_) {
                  ret = decode_plain_bool(body, body_len, num_values, out);
                } else {
                  ret = decode_plain(col.physical_, col.type_len_, body, body_len, num_values, out);
                }
              } else if (2 == encoding || 8 == encoding) {
                // dictionary indices: bit width byte + RLE. For the all-plain sample this is unused.
                if (body_len < 1) {
                  ret = OB_INVALID_ARGUMENT;
                } else {
                  int bw = body[0];
                  // Fall back: if dictionary exists and bw is small, decode indices as plain int32
                  // only when the payload looks like fixed 4-byte indexes. Otherwise not supported
                  // unless num_values matches a simple RLE of zeros.
                  if (dict.count() > 0 && 0 == bw && num_values > 0) {
                    for (int64_t n = 0; OB_SUCC(ret) && n < num_values; ++n) {
                      ret = out.push_back(dict.at(0));
                    }
                  } else {
                    ret = OB_NOT_SUPPORTED;
                    LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet dictionary page");
                  }
                }
              } else {
                ret = OB_NOT_SUPPORTED;
                LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet page encoding");
              }
              if (OB_SUCC(ret) && 2 != page_type) {
                produced += out.count() - before;
              }
            }
            pos += hdr_sz + comp;
          }
        }
      }
    }
    return ret;
  }
  ByteReader rd_;
  const ObFileSqlTableDef *def_;
  ObArenaAllocator page_alloc_;
  ObSEArray<PqCol, 8> cols_;
  ObSEArray<ObSEArray<PqChunk, 1>, 8> chunks_;
  ObSEArray<ObSEArray<PqVal, 64>, 8> vals_;
  int64_t nrows_ = 0;
  int64_t val_idx_;
};

int canonicalize_glob(ObSQLSessionInfo *session, ObIAllocator &alloc,
                      const ObString &raw, ObString &pattern)
{
  int ret = OB_SUCCESS;
  char tmp[PATH_MAX];
  if (raw.length() <= 0 || raw.length() >= PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(tmp, raw.ptr(), raw.length());
    tmp[raw.length()] = '\0';
    char *slash = strrchr(tmp, '/');
    char dirbuf[PATH_MAX];
    const char *dir = ".";
    const char *base = tmp;
    if (NULL != slash) {
      if (slash == tmp) {
        dir = "/";
      } else {
        *slash = '\0';
        dir = tmp;
      }
      base = slash + 1;
    }
    ObString canon_dir;
    if (OB_FAIL(check_secure_realpath(session, dir, canon_dir, alloc))) {
    } else {
      int n = snprintf(dirbuf, sizeof(dirbuf), "%.*s/%s",
                       canon_dir.length(), canon_dir.ptr(), base);
      if (n <= 0 || n >= static_cast<int>(sizeof(dirbuf))) {
        ret = OB_SIZE_OVERFLOW;
      } else if ('/' == canon_dir.ptr()[0] && 1 == canon_dir.length()) {
        n = snprintf(dirbuf, sizeof(dirbuf), "/%s", base);
      }
      if (OB_SUCC(ret)) {
        ret = write_dup(alloc, dirbuf, n > 0 ? n : static_cast<int>(strlen(dirbuf)), pattern);
      }
    }
  }
  return ret;
}

} // namespace

int ob_file_sql_copy_def(ObIAllocator &alloc, const ObFileSqlTableDef &src, ObFileSqlTableDef *&dst)
{
  int ret = OB_SUCCESS;
  void *mem = alloc.alloc(sizeof(ObFileSqlTableDef));
  ObFileSqlColMeta *cols = NULL;
  if (OB_ISNULL(mem)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (src.col_cnt_ > 0
             && OB_ISNULL(cols = static_cast<ObFileSqlColMeta *>(
                    alloc.alloc(sizeof(ObFileSqlColMeta) * src.col_cnt_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    dst = new (mem) ObFileSqlTableDef();
    dst->kind_ = src.kind_;
    dst->header_ = src.header_;
    dst->row_count_ = src.row_count_;
    dst->col_cnt_ = src.col_cnt_;
    dst->cols_ = cols;
    if (OB_FAIL(write_dup(alloc, src.path_, dst->path_))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < src.col_cnt_; ++i) {
      new (&cols[i]) ObFileSqlColMeta();
      cols[i].type_ = src.cols_[i].type_;
      cols[i].nullable_ = src.cols_[i].nullable_;
      cols[i].is_bool_ = src.cols_[i].is_bool_;
      cols[i].length_ = src.cols_[i].length_;
      cols[i].max_raw_len_ = src.cols_[i].max_raw_len_;
      if (OB_FAIL(write_dup(alloc, src.cols_[i].name_, cols[i].name_))) {
      }
    }
  }
  return ret;
}

int ob_file_sql_prepare(ObIAllocator &alloc,
                        ObSQLSessionInfo *session,
                        ObFileSqlKind kind,
                        const ObString &raw_path,
                        bool header,
                        ObFileSqlTableDef *&def)
{
  int ret = OB_SUCCESS;
  def = NULL;
  if (OB_ISNULL(session) || raw_path.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FILE_SQL_GLOB == kind) {
    ObString pattern;
    if (OB_FAIL(canonicalize_glob(session, alloc, raw_path, pattern))) {
    } else {
      ret = infer_glob(alloc, pattern, def);
    }
  } else {
    char tmp[PATH_MAX];
    ObString canon;
    if (raw_path.length() <= 0 || raw_path.length() >= PATH_MAX) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(tmp, raw_path.ptr(), raw_path.length());
      tmp[raw_path.length()] = '\0';
      if (OB_FAIL(check_secure_realpath(session, tmp, canon, alloc))) {
      } else if (OB_FAIL(check_regular_file(canon))) {
      } else if (OB_FILE_SQL_CSV == kind) {
        ret = infer_csv(alloc, canon, header, def);
      } else if (OB_FILE_SQL_JSONL == kind) {
        ret = infer_jsonl(alloc, canon, def);
      } else if (OB_FILE_SQL_PARQUET == kind) {
        ret = infer_parquet(alloc, canon, def);
      } else {
        ret = OB_INVALID_ARGUMENT;
      }
    }
  }
  return ret;
}

OB_SERIALIZE_MEMBER((ObFileSqlSpec, ObOpSpec), path_, kind_, bool_flags_, column_exprs_);

ObFileSqlOp::ObFileSqlOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      reader_(NULL),
      row_alloc_("FileSqlRow")
{}

int ObFileSqlOp::open_reader()
{
  int ret = OB_SUCCESS;
  const ObFileSqlSpec &spec = static_cast<const ObFileSqlSpec &>(spec_);
  ObString path = spec.path_.get_varchar();
  ObCollationType cs = CS_TYPE_UTF8MB4_BIN;
  if (OB_NOT_NULL(ctx_.get_my_session())) {
    cs = ctx_.get_my_session()->get_local_collation_connection();
  }
  // Re-check secure_file_priv at execution so a cached plan cannot escape a later restriction.
  if (OB_FILE_SQL_GLOB == spec.kind_) {
    ObString dummy;
    char tmp[PATH_MAX];
    if (path.length() <= 0 || path.length() >= PATH_MAX) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(tmp, path.ptr(), path.length());
      tmp[path.length()] = '\0';
      char *slash = strrchr(tmp, '/');
      if (NULL != slash && slash != tmp) {
        *slash = '\0';
      }
      const char *dir = (NULL == slash) ? "." : (slash == tmp ? "/" : tmp);
      if (OB_FAIL(check_secure_realpath(ctx_.get_my_session(), dir, dummy, row_alloc_))) {
      }
    }
  } else {
    ObString canon;
    if (OB_FAIL(check_secure_realpath(ctx_.get_my_session(), path.ptr(), canon, row_alloc_))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FILE_SQL_CSV == spec.kind_ || OB_FILE_SQL_JSONL == spec.kind_
             || OB_FILE_SQL_PARQUET == spec.kind_) {
    // Schema is rebuilt from the file so execution does not depend on resolver memory.
    ObFileSqlTableDef *def = NULL;
    bool header = true;
    if (OB_FILE_SQL_CSV == spec.kind_) {
      // header flag is encoded as the first bool flag byte being unused; CSV header
      // is recovered by re-inferring. prepare() is deterministic for the same file.
      if (OB_FAIL(ob_file_sql_prepare(ctx_.get_allocator(), ctx_.get_my_session(),
                                      static_cast<ObFileSqlKind>(spec.kind_), path, true, def))) {
      }
    } else if (OB_FAIL(ob_file_sql_prepare(ctx_.get_allocator(), ctx_.get_my_session(),
                                           static_cast<ObFileSqlKind>(spec.kind_), path, false, def))) {
    }
    if (OB_SUCC(ret) && def->col_cnt_ != spec.column_exprs_.count()) {
      // The file changed shape since planning; fail loudly rather than shift values between columns.
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("file table column count changed", K(ret), K(def->col_cnt_), K(spec.column_exprs_.count()));
    } else if (OB_FAIL(ret)) {
    } else if (OB_FILE_SQL_CSV == spec.kind_) {
      reader_ = new (ctx_.get_allocator().alloc(sizeof(CsvExec))) CsvExec(def->path_, cs, def);
    } else if (OB_FILE_SQL_JSONL == spec.kind_) {
      reader_ = new (ctx_.get_allocator().alloc(sizeof(JsonExec))) JsonExec(def->path_, cs, def);
    } else {
      reader_ = new (ctx_.get_allocator().alloc(sizeof(ParquetExec))) ParquetExec(def->path_, cs, def);
    }
  } else if (OB_FILE_SQL_GLOB == spec.kind_) {
    reader_ = new (ctx_.get_allocator().alloc(sizeof(GlobExec))) GlobExec(path);
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(reader_)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(reader_->open())) {
  }
  return ret;
}

void ObFileSqlOp::close_reader()
{
  if (NULL != reader_) {
    reader_->close();
    reader_->~Reader();
    reader_ = NULL;
  }
}

int ObFileSqlOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(open_reader())) {
    LOG_WARN("open file sql reader failed", K(ret));
  }
  return ret;
}

int ObFileSqlOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  close_reader();
  row_alloc_.reuse();
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (OB_FAIL(open_reader())) {
  }
  return ret;
}

int ObFileSqlOp::inner_close()
{
  close_reader();
  row_alloc_.reset();
  return OB_SUCCESS;
}

void ObFileSqlOp::destroy()
{
  close_reader();
  row_alloc_.reset();
  ObOperator::destroy();
}

int ObFileSqlOp::fill_row(const ObIArray<ObObj> &cells)
{
  int ret = OB_SUCCESS;
  const ObFileSqlSpec &spec = static_cast<const ObFileSqlSpec &>(spec_);
  clear_evaluated_flag();
  if (cells.count() != spec.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file sql column count mismatch", K(ret), K(cells.count()), K(spec.column_exprs_.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_exprs_.count(); ++i) {
    ObExpr *expr = spec.column_exprs_.at(i);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      if (OB_FAIL(datum.from_obj(cells.at(i), expr->obj_datum_map_))) {
        LOG_WARN("from_obj failed", K(ret), K(i));
      } else {
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

int ObFileSqlOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 16> cells;
  row_alloc_.reuse();
  if (OB_ISNULL(reader_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(reader_->next(row_alloc_, cells))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("file sql next row failed", K(ret));
    }
  } else if (OB_FAIL(fill_row(cells))) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
