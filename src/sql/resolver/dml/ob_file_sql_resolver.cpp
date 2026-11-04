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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/engine/basic/ob_file_sql_op.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace {

bool opt_name_is(const ParseNode *name, const char *lit)
{
  return OB_NOT_NULL(name) && OB_NOT_NULL(name->str_value_)
         && 0 == strncasecmp(name->str_value_, lit, strlen(lit))
         && static_cast<int64_t>(strlen(lit)) == name->str_len_;
}

int read_bool_node(const ParseNode *node, bool &out)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else if (T_BOOL == node->type_) {
    out = (0 != node->value_);
  } else if (T_INT == node->type_) {
    out = (0 != node->value_);
  } else {
    ret = OB_ERR_PARSER_SYNTAX;
  }
  return ret;
}

int read_int_node(const ParseNode *node, int64_t &out)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node) || T_INT != node->type_) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else {
    out = node->value_;
  }
  return ret;
}

} // namespace

int ObDMLResolver::resolve_file_sql_columns(const TableItem &table_item,
                                            ObIArray<ColumnItem> &col_items)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(stmt) || !table_item.is_file_sql_table()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_column_size(); ++i) {
      ColumnItem *item = stmt->get_column_item(i);
      if (OB_ISNULL(item)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (item->table_id_ == table_item.table_id_ && OB_FAIL(col_items.push_back(*item))) {
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_sql_table_item(const ParseNode &parse_tree, TableItem *&tbl_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  tbl_item = NULL;
  if (OB_ISNULL(stmt) || OB_ISNULL(allocator_) || OB_ISNULL(session_info_)
      || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_NOT_INIT;
  } else if (T_FILE_SQL_TABLE != parse_tree.type_ || parse_tree.num_child_ != 4
             || OB_ISNULL(parse_tree.children_[0]) || OB_ISNULL(parse_tree.children_[1])) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else {
    bool enabled = false;
    if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
      LOG_WARN("read enable_file_sql failed", K(ret));
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "local file SQL (set enable_file_sql = ON)");
    }
  }
  int64_t kind_val = 0;
  ObString path;
  bool header = true;
  ObFileSqlKind kind = OB_FILE_SQL_CSV;
  if (OB_FAIL(ret)) {
  } else {
    kind_val = parse_tree.children_[0]->value_;
    ParseNode *path_node = parse_tree.children_[1];
    if (OB_ISNULL(path_node->str_value_) || path_node->str_len_ <= 0) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      path.assign_ptr(path_node->str_value_, static_cast<int32_t>(path_node->str_len_));
    }
    if (1 == kind_val) {
      kind = OB_FILE_SQL_CSV;
    } else if (2 == kind_val || 3 == kind_val) {
      kind = OB_FILE_SQL_JSONL;
      header = false;
    } else if (4 == kind_val) {
      kind = OB_FILE_SQL_PARQUET;
      header = false;
    } else if (5 == kind_val) {
      kind = OB_FILE_SQL_GLOB;
      header = false;
    } else {
      ret = OB_ERR_PARSER_SYNTAX;
    }
  }
  ParseNode *opts = OB_SUCC(ret) ? parse_tree.children_[2] : NULL;
  if (OB_SUCC(ret) && OB_NOT_NULL(opts)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < opts->num_child_; ++i) {
      ParseNode *opt = opts->children_[i];
      if (OB_ISNULL(opt) || opt->num_child_ != 2 || OB_ISNULL(opt->children_[0])
          || OB_ISNULL(opt->children_[1])) {
        ret = OB_ERR_PARSER_SYNTAX;
      } else if (opt_name_is(opt->children_[0], "header")) {
        if (OB_FAIL(read_bool_node(opt->children_[1], header))) {
        }
      } else if (opt_name_is(opt->children_[0], "sample_size")) {
        int64_t sample = 0;
        if (OB_FAIL(read_int_node(opt->children_[1], sample))) {
        } else if (-1 != sample) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL sample_size other than -1");
        }
      } else if (opt_name_is(opt->children_[0], "format")) {
        ParseNode *fmt = opt->children_[1];
        if (OB_ISNULL(fmt) || OB_ISNULL(fmt->str_value_)) {
          ret = OB_ERR_PARSER_SYNTAX;
        } else if (OB_FILE_SQL_JSONL == kind) {
          if (0 != strncasecmp(fmt->str_value_, "newline_delimited", fmt->str_len_)
              && 0 != strncasecmp(fmt->str_value_, "ndjson", fmt->str_len_)
              && 0 != strncasecmp(fmt->str_value_, "jsonl", fmt->str_len_)) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "read_json format other than newline_delimited");
          }
        } else if (OB_FILE_SQL_CSV == kind) {
          if (0 != strncasecmp(fmt->str_value_, "csv", fmt->str_len_)) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "read_csv format other than csv");
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "format option on this file table");
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL option");
      }
    }
  }
  ObFileSqlTableDef *def = NULL;
  TableItem *item = NULL;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ob_file_sql_prepare(*allocator_, session_info_, kind, path, header, def))) {
    LOG_WARN("prepare file sql table failed", K(ret), K(path), K(kind));
    if (OB_FILE_NOT_EXIST == ret) {
      LOG_USER_ERROR(OB_FILE_NOT_EXIST);
    }
  } else if (OB_ISNULL(item = stmt->create_table_item(*allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ParseNode *alias_node = parse_tree.children_[3];
    ObString alias;
    const char *fallback = "read_csv";
    if (OB_FILE_SQL_JSONL == kind) {
      fallback = "read_json";
    } else if (OB_FILE_SQL_PARQUET == kind) {
      fallback = "read_parquet";
    } else if (OB_FILE_SQL_GLOB == kind) {
      fallback = "glob";
    }
    if (OB_NOT_NULL(alias_node) && OB_NOT_NULL(alias_node->str_value_) && alias_node->str_len_ > 0) {
      alias.assign_ptr(alias_node->str_value_, static_cast<int32_t>(alias_node->str_len_));
    } else if (OB_FAIL(ob_write_string(*allocator_, ObString(fallback), alias))) {
    }
    if (OB_SUCC(ret)) {
      item->table_name_ = alias;
      item->alias_name_ = alias;
      item->type_ = TableItem::FILE_SQL_TABLE;
      item->table_id_ = generate_table_id();
      item->file_sql_def_ = def;
      item->is_system_table_ = false;
      if (OB_FAIL(stmt->add_table_item(session_info_, item))) {
      }
    }
  }
  ObCollationType cs_type = CS_TYPE_UTF8MB4_GENERAL_CI;
  if (OB_SUCC(ret)) {
    session_info_->get_collation_connection(cs_type);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < def->col_cnt_; ++i) {
    const ObFileSqlColMeta &col = def->cols_[i];
    ObColumnRefRawExpr *col_expr = NULL;
    ColumnItem column_item;
    ObRawExprResType result_type;
    ObObjMeta meta;
    ObAccuracy accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[col.type_];
    if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_REF_COLUMN, col_expr))) {
    } else if (OB_ISNULL(col_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      meta.set_type(col.type_);
      if (ObVarcharType == col.type_) {
        meta.set_collation_type(cs_type);
        meta.set_collation_level(CS_LEVEL_IMPLICIT);
        accuracy.set_length(col.length_ > 0 ? col.length_ : 1);
      } else {
        meta.set_collation_type(CS_TYPE_BINARY);
        meta.set_collation_level(CS_LEVEL_NUMERIC);
      }
      if (col.is_bool_) {
        accuracy.set_precision(1);
        accuracy.set_scale(0);
      }
      result_type.set_meta(meta);
      result_type.set_accuracy(accuracy);
      if (!col.nullable_) {
        result_type.set_result_flag(NOT_NULL_FLAG);
      }
      col_expr->set_ref_id(item->table_id_, OB_APP_MIN_COLUMN_ID + i);
      col_expr->set_result_type(result_type);
      col_expr->set_column_attr(item->get_object_name(), col.name_);
      col_expr->set_database_name(item->database_name_);
      column_item.expr_ = col_expr;
      column_item.table_id_ = item->table_id_;
      column_item.column_id_ = col_expr->get_column_id();
      column_item.column_name_ = col_expr->get_column_name();
      if (OB_FAIL(col_expr->extract_info())) {
      } else if (OB_FAIL(stmt->add_column_item(column_item))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    tbl_item = item;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
