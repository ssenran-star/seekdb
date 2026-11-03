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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_OP_H_

#include "sql/engine/ob_operator.h"

namespace oceanbase
{
namespace sql
{

enum ObFileSqlKind
{
  OB_FILE_SQL_CSV = 1,
  OB_FILE_SQL_JSONL = 2,
  OB_FILE_SQL_PARQUET = 3,
  OB_FILE_SQL_GLOB = 4,
};

struct ObFileSqlColMeta
{
  common::ObString name_;
  common::ObObjType type_;
  bool nullable_;
  bool is_bool_;
  int32_t length_;
  int32_t max_raw_len_;

  ObFileSqlColMeta()
      : name_(), type_(common::ObNullType), nullable_(true), is_bool_(false),
        length_(0), max_raw_len_(0) {}
};

// Schema of a local file table. Owned by the statement allocator.
struct ObFileSqlTableDef
{
  ObFileSqlKind kind_;
  common::ObString path_;
  bool header_;
  int64_t row_count_;
  ObFileSqlColMeta *cols_;
  int64_t col_cnt_;

  ObFileSqlTableDef()
      : kind_(OB_FILE_SQL_CSV), path_(), header_(true), row_count_(0),
        cols_(NULL), col_cnt_(0) {}
};

class ObSQLSessionInfo;

// Open the path under secure_file_priv and infer a schema without retaining rows.
int ob_file_sql_prepare(common::ObIAllocator &alloc,
                        ObSQLSessionInfo *session,
                        ObFileSqlKind kind,
                        const common::ObString &raw_path,
                        bool header,
                        ObFileSqlTableDef *&def);

int ob_file_sql_copy_def(common::ObIAllocator &alloc,
                         const ObFileSqlTableDef &src,
                         ObFileSqlTableDef *&dst);

class ObFileSqlSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileSqlSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
      : ObOpSpec(alloc, type),
        path_(),
        kind_(OB_FILE_SQL_CSV),
        bool_flags_(),
        column_exprs_(alloc)
  {}

  common::ObObj path_;
  int64_t kind_;
  // One byte per column: '1' boolean, '0' otherwise. Types come from column exprs.
  common::ObObj bool_flags_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
};

class ObFileSqlOp : public ObOperator
{
public:
  ObFileSqlOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input);
  virtual ~ObFileSqlOp() {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  int open_reader();
  void close_reader();
  int fill_row(const common::ObIArray<common::ObObj> &cells);

  struct Reader;
  Reader *reader_;
  common::ObArenaAllocator row_alloc_;
};

} // namespace sql
} // namespace oceanbase

#endif
