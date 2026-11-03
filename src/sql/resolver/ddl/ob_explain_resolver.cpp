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
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    // DESCRIBE/DESC <select> returns result-set metadata. EXPLAIN keeps the plan.
    if (1 == parse_tree.value_ && OB_NOT_NULL(child_node) && T_SELECT == child_node->type_) {
      ObDescribeQueryResolver describe_resolver(params_);
      ParseNode describe_node;
      ParseNode *describe_child[1] = {child_node};
      memset(&describe_node, 0, sizeof(describe_node));
      describe_node.type_ = T_DESCRIBE_QUERY;
      describe_node.num_child_ = 1;
      describe_node.children_ = describe_child;
      if (OB_FAIL(describe_resolver.resolve(describe_node))) {
        LOG_WARN("resolve DESCRIBE SELECT failed", K(ret));
      } else {
        stmt_ = describe_resolver.get_basic_stmt();
      }
      return ret;
    }
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}

namespace {

const char *describe_type_name(const ObRawExpr *expr)
{
  const char *name = "VARCHAR";
  if (OB_ISNULL(expr)) {
    name = "NULL";
  } else {
    const ObRawExprResType &t = expr->get_result_type();
    switch (t.get_type()) {
      case ObTinyIntType:
        name = (1 == t.get_precision()) ? "BOOLEAN" : "TINYINT";
        break;
      case ObSmallIntType: name = "SMALLINT"; break;
      case ObMediumIntType: name = "MEDIUMINT"; break;
      case ObInt32Type: name = "INT"; break;
      case ObIntType: name = "BIGINT"; break;
      case ObUInt64Type: name = "BIGINT UNSIGNED"; break;
      case ObFloatType: name = "FLOAT"; break;
      case ObDoubleType: name = "DOUBLE"; break;
      case ObNumberType: name = "DECIMAL"; break;
      case ObDateTimeType: name = "DATETIME"; break;
      case ObTimestampType: name = "TIMESTAMP"; break;
      case ObDateType: name = "DATE"; break;
      case ObTimeType: name = "TIME"; break;
      case ObYearType: name = "YEAR"; break;
      case ObVarcharType:
      case ObCharType: name = "VARCHAR"; break;
      case ObJsonType: name = "JSON"; break;
      default: name = "VARCHAR"; break;
    }
  }
  return name;
}

int append_sql_literal(ObSqlString &sql, const ObString &text)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int32_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
    char c = text.ptr()[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if (OB_FAIL(sql.append(&c, 1))) {
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

} // namespace

int ObDescribeQueryResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  if (T_DESCRIBE_QUERY != parse_tree.type_ || 1 != parse_tree.num_child_
      || OB_ISNULL(parse_tree.children_[0]) || OB_ISNULL(allocator_)
      || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObResolver resolver(params_);
    ObStmt *child = NULL;
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *parse_tree.children_[0], child))) {
      LOG_WARN("resolve described query failed", K(ret));
    } else if (OB_ISNULL(child) || !child->is_select_stmt()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "DESCRIBE of a non-select statement");
    } else {
      ObSelectStmt *sel = static_cast<ObSelectStmt *>(child);
      ObSqlString sql;
      if (0 == sel->get_select_item_size()) {
        ret = sql.append("SELECT CAST(NULL AS CHAR(128)) AS `column_name`, "
                         "CAST(NULL AS CHAR(64)) AS `column_type`, "
                         "CAST(NULL AS CHAR(3)) AS `null`, "
                         "CAST(NULL AS CHAR(64)) AS `key`, "
                         "CAST(NULL AS CHAR(64)) AS `default`, "
                         "CAST(NULL AS CHAR(128)) AS `extra` FROM DUAL WHERE 0");
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < sel->get_select_item_size(); ++i) {
        const SelectItem &item = sel->get_select_item(i);
        ObString col_name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
        const char *type_name = describe_type_name(item.expr_);
        const char *null_name = "YES";
        if (OB_NOT_NULL(item.expr_) && item.expr_->get_result_type().has_result_flag(NOT_NULL_FLAG)) {
          null_name = "NO";
        }
        if (0 == i) {
          if (OB_FAIL(sql.append("SELECT "))) {
          }
        } else if (OB_FAIL(sql.append(" UNION ALL SELECT "))) {
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(append_sql_literal(sql, col_name))) {
        } else if (OB_FAIL(sql.append(" AS `column_name`, "))) {
        } else if (OB_FAIL(sql.append("'"))) {
        } else if (OB_FAIL(sql.append(type_name))) {
        } else if (OB_FAIL(sql.append("' AS `column_type`, '"))) {
        } else if (OB_FAIL(sql.append(null_name))) {
        } else if (OB_FAIL(sql.append("' AS `null`, NULL AS `key`, NULL AS `default`, NULL AS `extra` FROM DUAL"))) {
        }
      }
      ParseResult parse_result;
      ObParser parser(*allocator_, session_info_->get_sql_mode(), session_info_->get_charsets4parser());
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(parser.parse(ObString(sql.length(), sql.ptr()), parse_result))) {
        LOG_WARN("parse describe projection failed", K(ret), K(sql));
      } else if (OB_ISNULL(parse_result.result_tree_)
                 || T_STMT_LIST != parse_result.result_tree_->type_
                 || parse_result.result_tree_->num_child_ < 1
                 || OB_ISNULL(parse_result.result_tree_->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObResolver resolver2(params_);
        ObStmt *out = NULL;
        if (OB_FAIL(resolver2.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                      *parse_result.result_tree_->children_[0], out))) {
          LOG_WARN("resolve describe projection failed", K(ret));
        } else if (OB_ISNULL(out)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          stmt_ = out;
        }
      }
    }
  }
  return ret;
}
