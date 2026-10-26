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
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/parser/ob_parser.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/string/ob_string_buffer.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
namespace {
bool file_description_has_scan(const ObSelectStmt &query, int depth = 0)
{
  bool found = false;
  if (depth < 64) {
    for (int64_t i = 0; !found && i < query.get_table_size(); ++i) {
      const TableItem *table = query.get_table_item(i);
      if (table != nullptr) {
        found = table->is_function_table() && table->function_table_expr_ != nullptr
                && table->function_table_expr_->get_expr_type() == T_FUN_SYS_FILE_SCAN;
        if (!found && table->is_generated_table() && table->ref_query_ != nullptr) {
          found = file_description_has_scan(*table->ref_query_, depth + 1);
        }
      }
    }
  }
  return found;
}
}

int ObExplainResolver::resolve_file_description(const ObSelectStmt &query)
{
  int ret = OB_SUCCESS;
  ObJsonArray metadata(allocator_);
  for (int64_t i = 0; OB_SUCC(ret) && i < query.get_select_item_size(); ++i) {
    const SelectItem &item = query.get_select_item(i);
    const ObExprResType &type = item.expr_->get_result_type();
    const char *type_name = nullptr;
    switch (type.get_type()) {
      case ObNullType: type_name = "NULL"; break;
      case ObTinyIntType: type_name = "BOOLEAN"; break;
      case ObIntType: type_name = "BIGINT"; break;
      case ObDoubleType: type_name = "DOUBLE"; break;
      case ObDateType: type_name = "DATE"; break;
      case ObDateTimeType: type_name = "DATETIME"; break;
      case ObVarcharType: type_name = "VARCHAR"; break;
      default: type_name = ob_obj_type_str(type.get_type()); break;
    }
    ObJsonObject *column = OB_NEWx(ObJsonObject, allocator_, allocator_);
    ObJsonString *name = OB_NEWx(ObJsonString, allocator_, item.alias_name_);
    ObJsonString *kind = OB_NEWx(ObJsonString, allocator_, ObString(type_name));
    ObJsonString *nullable = OB_NEWx(ObJsonString, allocator_, ObString(type.is_not_null_for_read() ? "NO" : "YES"));
    if (column == nullptr || name == nullptr || kind == nullptr || nullable == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(column->add("name", name))) {
    } else if (OB_FAIL(column->add("type", kind))) {
    } else if (OB_FAIL(column->add("nullable", nullable))) {
    } else { ret = metadata.append(column); }
  }
  ObStringBuffer json(allocator_);
  ObSqlString sql;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(metadata.print(json, 0))) {
  } else if (OB_FAIL(sql.append("SELECT t.`Field`,t.`Type`,t.`Null`,'' AS `Key`,NULL AS `Default`,'' AS `Extra` "
                "FROM JSON_TABLE(CONVERT(X'"))) {
  }
  // Encode the complete metadata document as a hex literal, including names.
  // SQL modes and punctuation in column names cannot change the generated SQL.
  for (int64_t i = 0; OB_SUCC(ret) && i < json.length(); ++i) { ret = sql.append_fmt("%02x", static_cast<unsigned char>(json.ptr()[i])); }
  if (OB_SUCC(ret)) {
    ret = sql.append("' USING utf8mb4),'$[*]' COLUMNS (`Field` VARCHAR(1024) PATH '$.name',"
                     "`Type` VARCHAR(64) PATH '$.type',`Null` VARCHAR(3) PATH '$.nullable')) t");
  }
  ObParser parser(*allocator_, session_info_->get_sql_mode());
  ParseResult result;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(parser.parse(sql.string(), result))) {
  } else if (result.result_tree_ == nullptr || result.result_tree_->num_child_ != 1) { ret = OB_ERR_UNEXPECTED; }
  else {
    ObSelectResolver resolver(params_);
    if (OB_FAIL(resolver.resolve(*result.result_tree_->children_[0]))) {
    } else { stmt_ = resolver.get_stmt(); }
  }
  return ret;
}

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
        if (parse_tree.value_ == 1 && child_stmt->is_select_stmt()
            && file_description_has_scan(*static_cast<ObSelectStmt *>(child_stmt))) {
          return resolve_file_description(*static_cast<ObSelectStmt *>(child_stmt));
        }
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
