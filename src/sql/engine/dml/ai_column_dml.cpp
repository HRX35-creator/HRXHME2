/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/session/ob_sql_session_info.h"
#include "query/session/ob_session_inner_sql.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/ob_server_struct.h"
#include "share/ai_service/ai_column_sql.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "sql/ob_sql_context.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include <cctype>

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{
namespace
{
// Inner maintenance joins the caller's transaction while preserving the outer
// statement's nested-session state, counters and execution context.
class AiColumnConnection
{
public:
  explicit AiColumnConnection(ObSQLSessionInfo *session) : connection_(nullptr), nested_(false)
  {
    error_ = query::ObInnerSQLConnectionAccess::create_spi_connection_with_external_session(session, guard_);
    if (error_ == OB_SUCCESS) {
      connection_ = as_inner_sql_connection(guard_.get_ptr());
      if (connection_ == nullptr) { error_ = OB_ERR_UNEXPECTED; }
      else if (session->has_start_stmt()) {
        error_ = connection_->begin_nested_session(saved_session_, saved_connection_, true);
        nested_ = error_ == OB_SUCCESS;
      }
    }
  }
  ~AiColumnConnection() { close(); }
  int close()
  {
    int ret = OB_SUCCESS;
    if (nested_) {
      nested_ = false;
      ret = connection_->end_nested_session(saved_session_, saved_connection_);
    }
    return ret;
  }
  int execute_write(const ObSqlString &sql, int64_t &affected)
  { return error_ == OB_SUCCESS ? connection_->execute_write(sql.ptr(), affected) : error_; }
  int execute_read(const ObSqlString &sql, ObISQLClient::ReadResult &result)
  { return error_ == OB_SUCCESS ? connection_->execute_read(sql.ptr(), result) : error_; }
private:
  sqlclient::ObISQLConnectionGuard guard_;
  ObIInnerSQLConnection *connection_;
  ObSQLSessionInfo::StmtSavedValue saved_session_;
  ObIInnerSQLConnection::SavedValue saved_connection_;
  bool nested_;
  int error_;
};

int row_position(const ObDMLBaseCtDef &definition, uint64_t id)
{
  int pos = -1;
  for (int64_t i = 0; pos < 0 && i < definition.column_ids_.count(); ++i) {
    if (definition.column_ids_.at(i) == id) { pos = static_cast<int>(i); }
  }
  return pos;
}

int row_predicate(const ObDMLBaseCtDef &definition, const ObTableSchema &table,
                  const ExprFixedArray &row, ObEvalCtx &ctx, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &key = table.get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < key.get_size(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema(key.get_column(i)->column_id_);
    const int pos = column == nullptr ? -1 : row_position(definition, column->get_column_id());
    ObDatum *datum = nullptr;
    if (column == nullptr || pos < 0 || pos >= row.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (i > 0 && OB_FAIL(sql.append(" AND "))) {
    } else if (OB_FAIL(ai_column_identifier(sql, column->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (OB_FAIL(row.at(pos)->eval(ctx, datum))) {
    } else if (datum->is_null()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (ob_is_string_type(column->get_data_type())) {
      ret = ai_column_literal(sql, datum->get_string());
    } else {
      ObObj value;
      char *buffer = nullptr;
      int64_t size = 0;
      int64_t written = 0;
      if (OB_FAIL(datum->to_obj(value, row.at(pos)->obj_meta_))) {
      } else if (OB_FAIL(value.print_sql_literal(buffer, size, written, ctx.exec_ctx_.get_allocator()))) {
      } else {
        ret = sql.append(buffer, written);
      }
    }
  }
  if (OB_SUCC(ret) && key.get_size() == 0) { ret = OB_NOT_SUPPORTED; }
  return ret;
}

int changed_column(const ObDMLBaseCtDef &definition, uint64_t id, ObEvalCtx &ctx, bool &changed)
{
  int ret = OB_SUCCESS;
  int pos = row_position(definition, id);
  ObDatum *before = nullptr;
  ObDatum *after = nullptr;
  if (pos < 0) {
    // A minimal UPDATE row omits untouched columns, including AI dependencies.
    changed = false;
  } else if (pos >= definition.old_row_.count() || pos >= definition.new_row_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(definition.old_row_.at(pos)->eval(ctx, before))) {
  } else if (OB_FAIL(definition.new_row_.at(pos)->eval(ctx, after))) {
  } else {
    changed = !ObDatum::binary_equal(*before, *after);
  }
  return ret;
}

int save_change(ObExecContext &ctx, const ObSqlString &invalidate,
                const ObSqlString &cleanup, const ObSqlString &enqueue)
{
  int ret = OB_SUCCESS;
  AiColumnChange change;
  if (OB_FAIL(ob_write_string(ctx.get_allocator(), invalidate.string(), change.invalidate_sql_))) {
  } else if (OB_FAIL(ob_write_string(ctx.get_allocator(), cleanup.string(), change.cleanup_sql_))) {
  } else if (OB_FAIL(ob_write_string(ctx.get_allocator(), enqueue.string(), change.enqueue_sql_))) {
  } else {
    ret = ctx.get_ai_column_changes().push_back(change);
  }
  return ret;
}
} // namespace

int AiColumnDml::check_strict_read(ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (session == nullptr || (session->is_inner() && !session->is_user_session()) || AiColumnExecutionGuard::active()) { return ret; }
  const ObPhysicalPlan *plan = ctx.get_physical_plan_ctx()->get_phy_plan();
  if (plan->ai_column_references_.empty()) { return ret; }
  ObObj enabled;
  if (OB_FAIL(session->get_sys_variable(SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) {
  } else if (enabled.get_int() != 0) {
    AiColumnExecutionGuard maintenance;
    AiColumnConnection connection(session);
    ObSchemaGetterGuard &schema_guard = *ctx.get_sql_ctx()->schema_guard_;
    for (int64_t i = 0; OB_SUCC(ret) && i < plan->ai_column_references_.count(); ++i) {
      const AiColumnReference &ref = plan->ai_column_references_.at(i);
      const ObTableSchema *table = nullptr;
      const ObDatabaseSchema *database = nullptr;
      const ObColumnSchemaV2 *column = nullptr;
      AiColumnDefinition ai;
      ObSqlString sql;
      ObISQLClient::ReadResult result;
      if (OB_FAIL(schema_guard.get_table_schema(ref.table_id_, table))) {
      } else if (table == nullptr || (column = table->get_column_schema(ref.column_id_)) == nullptr) {
        ret = OB_SCHEMA_EAGAIN;
      } else if (OB_FAIL(column->get_ai_column_definition(ai))) {
      } else if (OB_FAIL(schema_guard.get_database_schema(table->get_database_id(), database))) {
      } else if (database == nullptr) { ret = OB_SCHEMA_EAGAIN; }
      else if (OB_FAIL(sql.append("SELECT 1 FROM "))) {
      } else if (OB_FAIL(ai_column_identifier(sql, database->get_database_name_str()))) {
      } else if (OB_FAIL(sql.append("."))) {
      } else if (OB_FAIL(ai_column_identifier(sql, table->get_table_name_str()))) {
      } else if (OB_FAIL(sql.append(" WHERE "))) {
      } else if (OB_FAIL(ai_column_identifier(sql, table->get_column_schema(ai.state_column_id_)->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append(" IS NULL OR "))) {
      } else if (OB_FAIL(ai_column_identifier(sql, table->get_column_schema(ai.state_column_id_)->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append("<>1 LIMIT 1"))) {
      } else if (OB_FAIL(connection.execute_read(sql, result))) {
      } else if (result.mysql_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(result.mysql_result()->next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      } else {
        ret = OB_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, ref.table_id_, ref.column_id_);
      }
    }
    const int restore_ret = connection.close();
    if (OB_SUCC(ret)) { ret = restore_ret; }
  }
  return ret;
}

int AiColumnDml::retry(ObExecContext &ctx, uint64_t table_id, uint64_t column_id,
                        const ObString &predicate)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  // Command execution releases the resolver's guard before entering the executor.
  ObSchemaGetterGuard schema_guard;
  const ObColumnSchemaV2 *column = nullptr;
  AiColumnDefinition ai;
  ObSqlString sql;
  ObSqlString status;
  if (GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table))) {
  } else if (table == nullptr || (column = table->get_column_schema(column_id)) == nullptr) {
    ret = OB_SCHEMA_EAGAIN;
  } else if (!column->is_ai_column()) { ret = OB_NOT_SUPPORTED; }
  else if (OB_FAIL(column->get_ai_column_definition(ai))) {
  } else if (OB_FAIL(schema_guard.get_database_schema(table->get_database_id(), database))) {
  } else if (database == nullptr || table->get_rowkey_info().get_size() == 0) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(status.append("(CASE "))) {
  } else if (OB_FAIL(ai_column_identifier(status, table->get_column_schema(ai.state_column_id_)->get_column_name_str()))) {
  } else if (OB_FAIL(status.append(" WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END)"))) {
  } else if (OB_FAIL(sql.append("UPDATE "))) {
  } else if (OB_FAIL(ai_column_identifier(sql, database->get_database_name_str()))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(ai_column_identifier(sql, table->get_table_name_str()))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  } else {
    const ObString key = table->get_column_schema(table->get_rowkey_info().get_column(0)->column_id_)->get_column_name_str();
    if (OB_FAIL(ai_column_identifier(sql, key))) {
    } else if (OB_FAIL(sql.append("="))) {
    } else if (OB_FAIL(ai_column_identifier(sql, key))) {
    } else { ret = sql.append(" "); }
  }
  // The grammar has already validated WHERE. Preserve quoted identifiers,
  // strings and comments while expanding only unquoted STATUS identifiers.
  char quote = 0;
  bool line_comment = false;
  bool block_comment = false;
  char previous = 0;
  const bool backslash_escapes = !(ctx.get_my_session()->get_sql_mode() & SMO_NO_BACKSLASH_ESCAPES);
  for (int64_t i = 0; OB_SUCC(ret) && i < predicate.length();) {
    const char c = predicate.ptr()[i];
    const char next = i + 1 < predicate.length() ? predicate.ptr()[i + 1] : 0;
    if (line_comment) {
      if (c == '\n' || c == '\r') { line_comment = false; }
    } else if (block_comment) {
      if (c == '*' && next == '/') {
        ret = sql.append(predicate.ptr() + i, 2); i += 2; block_comment = false; continue;
      }
    } else if (quote != 0) {
      if (c == '\\' && backslash_escapes && quote != '`' && next != 0) {
        ret = sql.append(predicate.ptr() + i, 2); i += 2; continue;
      } else if (c == quote) {
        if (next == quote) { ret = sql.append(predicate.ptr() + i, 2); i += 2; continue; }
        quote = 0;
      }
    } else if (c == '\'' || c == '"' || c == '`') { quote = c; }
    else if (c == '/' && next == '*') { block_comment = true; }
    else if (c == '#' || (c == '-' && next == '-' && i + 2 < predicate.length()
                           && std::isspace(static_cast<unsigned char>(predicate.ptr()[i + 2])))) { line_comment = true; }
    else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 128) {
      const int64_t begin = i;
      while (i < predicate.length()) {
        const unsigned char ch = predicate.ptr()[i];
        if (!std::isalnum(ch) && ch != '_' && ch != '$' && ch < 128) { break; }
        ++i;
      }
      ObString token(i - begin, predicate.ptr() + begin);
      int64_t after = i;
      while (after < predicate.length() && std::isspace(static_cast<unsigned char>(predicate.ptr()[after]))) { ++after; }
      if (token.case_compare("status") == 0 && previous != '.'
          && (after == predicate.length() || predicate.ptr()[after] != '.')) { ret = sql.append(status.string()); }
      else { ret = sql.append(token); }
      previous = 'a';
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(c)) && !line_comment && !block_comment) { previous = c; }
    ret = sql.append(predicate.ptr() + i, 1);
    ++i;
  }
  if (OB_SUCC(ret)) {
    AiColumnRetryGuard retry_scope(table_id, column_id);
    AiColumnConnection connection(ctx.get_my_session());
    int64_t affected = 0;
    ret = connection.execute_write(sql, affected);
    const int restore_ret = connection.close();
    if (OB_SUCC(ret)) { ret = restore_ret; }
  }
  return ret;
}

int AiColumnDml::record_row(const ObDMLBaseCtDef &definition, ObEvalCtx &eval_ctx,
                           ObExecContext &ctx, bool inserting, bool deleting)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (!definition.is_primary_index_ || AiColumnExecutionGuard::active()
      || (session != nullptr && session->is_inner() && !session->is_user_session() && !AiColumnRetryGuard::active())) {
    return ret;
  }
  const ObTableSchema *table = nullptr;
  ObSchemaGetterGuard *guard = ctx.get_sql_ctx() == nullptr ? nullptr : ctx.get_sql_ctx()->schema_guard_;
  if (guard == nullptr || session == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(guard->get_table_schema(definition.das_base_ctdef_.index_tid_, table))) {
  } else if (table == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  }
  ObSEArray<const ObColumnSchemaV2 *, 4> columns;
  bool primary_key_changed = false;
  bool has_ai = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    has_ai |= table->get_column_schema_by_idx(i)->is_ai_column();
  }
  if (OB_FAIL(ret) || !has_ai) { return ret; }
  if (definition.das_base_ctdef_.is_ignore_) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "IGNORE writes to tables containing AI columns");
    return ret;
  }
  if (!inserting && !deleting) {
    for (int64_t i = 0; OB_SUCC(ret) && !primary_key_changed && i < table->get_rowkey_info().get_size(); ++i) {
      ret = changed_column(definition, table->get_rowkey_info().get_column(i)->column_id_, eval_ctx, primary_key_changed);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (column->is_ai_column()) {
      AiColumnDefinition ai;
      const uint64_t retry_column = AiColumnRetryGuard::column(table->get_table_id());
      bool changed = retry_column == OB_INVALID_ID ? inserting || deleting || primary_key_changed
                                                   : retry_column == column->get_column_id();
      if (OB_FAIL(column->get_ai_column_definition(ai))) {
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && !changed && retry_column == OB_INVALID_ID && j < ai.dependency_ids_.count(); ++j) {
          ret = changed_column(definition, ai.dependency_ids_.at(j), eval_ctx, changed);
        }
        if (OB_SUCC(ret) && changed) { ret = columns.push_back(column); }
      }
    }
  }
  if (OB_FAIL(ret) || columns.empty()) { return ret; }
  ObSqlString key;
  ObSqlString old_key;
  ObSqlString qualified;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) {
  } else if (database == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ai_column_identifier(qualified, database->get_database_name_str()))) {
  } else if (OB_FAIL(qualified.append("."))) {
  } else if (OB_FAIL(ai_column_identifier(qualified, table->get_table_name_str()))) {
  } else if (OB_FAIL(row_predicate(definition, *table,
                                  deleting ? definition.old_row_ : definition.new_row_, eval_ctx, key))) {
  } else if (!inserting && OB_FAIL(row_predicate(definition, *table, definition.old_row_, eval_ctx, old_key))) {
  }
  char uuid[UuidCommon::LENGTH_UUID + 1] = {};
  ObString request;
  if (OB_SUCC(ret) && !deleting) {
    if (OB_FAIL(ObExprUuid::gen_server_uuid(uuid, UuidCommon::LENGTH_UUID))) {
    } else { request.assign_ptr(uuid, UuidCommon::LENGTH_UUID); }
  }
  ObSqlString invalidate;
  ObSqlString cleanup;
  ObSqlString enqueue;
  if (OB_SUCC(ret)) {
    ret = cleanup.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate IN (", table->get_table_id());
    if (OB_SUCC(ret)) { ret = ai_column_literal(cleanup, key.string()); }
    if (OB_SUCC(ret) && !old_key.empty()) {
      if (OB_FAIL(cleanup.append(","))) {
      } else { ret = ai_column_literal(cleanup, old_key.string()); }
    }
    if (OB_SUCC(ret)) { ret = cleanup.append(") AND column_id IN ("); }
  }
  if (OB_SUCC(ret) && !deleting) {
    if (OB_FAIL(invalidate.append("UPDATE "))) {
    } else if (OB_FAIL(invalidate.append(qualified.string()))) {
    } else if (OB_FAIL(invalidate.append(" SET "))) {
    } else {
      ret = enqueue.append("INSERT INTO oceanbase.__all_ai_column_task(table_id,column_id,request_id,rowkey_predicate,definition_version,state,retry_count,max_retries,eligible_ts) VALUES ");
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const ObColumnSchemaV2 &column = *columns.at(i);
    AiColumnDefinition ai;
    if (OB_FAIL(column.get_ai_column_definition(ai))) {
    } else if (i > 0 && OB_FAIL(cleanup.append(","))) {
    } else if (OB_FAIL(cleanup.append_fmt("%lu", column.get_column_id()))) {
    } else if (!deleting) {
      const ObColumnSchemaV2 *state = table->get_column_schema(ai.state_column_id_);
      const ObColumnSchemaV2 *req = table->get_column_schema(ai.request_column_id_);
      const ObColumnSchemaV2 *spec = table->get_column_schema(ai.result_spec_column_id_);
      if (state == nullptr || req == nullptr || spec == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else if (i > 0 && OB_FAIL(invalidate.append(","))) {
      } else if (OB_FAIL(ai_column_identifier(invalidate, column.get_column_name_str()))) {
      } else if (OB_FAIL(invalidate.append("=NULL,"))) {
      } else if (OB_FAIL(ai_column_identifier(invalidate, state->get_column_name_str()))) {
      } else if (OB_FAIL(invalidate.append("=0,"))) {
      } else if (OB_FAIL(ai_column_identifier(invalidate, req->get_column_name_str()))) {
      } else if (OB_FAIL(invalidate.append("="))) {
      } else if (OB_FAIL(ai_column_literal(invalidate, request))) {
      } else if (OB_FAIL(invalidate.append(","))) {
      } else if (OB_FAIL(ai_column_identifier(invalidate, spec->get_column_name_str()))) {
      } else if (OB_FAIL(invalidate.append_fmt("=%ld", ai.definition_version_))) {
      } else if (i > 0 && OB_FAIL(enqueue.append(","))) {
      } else if (OB_FAIL(enqueue.append_fmt("(%lu,%lu,", table->get_table_id(), column.get_column_id()))) {
      } else if (OB_FAIL(ai_column_literal(enqueue, request))) {
      } else if (OB_FAIL(enqueue.append(","))) {
      } else if (OB_FAIL(ai_column_literal(enqueue, key.string()))) {
      } else {
        ret = enqueue.append_fmt(",%ld,0,0,%ld,NOW(6))", ai.definition_version_, ai.max_retries_);
      }
    }
  }
  if (OB_SUCC(ret)) { ret = cleanup.append(")"); }
  if (OB_SUCC(ret) && !deleting) {
    if (OB_FAIL(invalidate.append(" WHERE "))) {
    } else if (OB_FAIL(invalidate.append(key.string()))) {
    } else if (inserting) {
      AiColumnDefinition ai;
      if (OB_FAIL(columns.at(0)->get_ai_column_definition(ai))) {
      } else if (OB_FAIL(invalidate.append(" AND "))) {
      } else if (OB_FAIL(ai_column_identifier(invalidate, table->get_column_schema(ai.request_column_id_)->get_column_name_str()))) {
      } else { ret = invalidate.append(" IS NULL"); }
    }
  }
  if (OB_SUCC(ret)) { ret = save_change(ctx, invalidate, cleanup, enqueue); }
  if (OB_FAIL(ret)) { LOG_WARN("ai column record row failed", K(ret), K(invalidate), K(cleanup), K(enqueue), K(inserting), K(deleting)); }
  return ret;
}

int AiColumnDml::finish_statement(ObExecContext &ctx, bool rollback)
{
  int ret = OB_SUCCESS;
  auto &changes = ctx.get_ai_column_changes();
  if (!changes.empty() && !rollback) {
    AiColumnExecutionGuard guard;
    AiColumnConnection connection(ctx.get_my_session());
    for (int64_t i = 0; OB_SUCC(ret) && i < changes.count(); ++i) {
      const AiColumnChange &change = changes.at(i);
      int64_t affected = 1;
      ObSqlString sql;
      if (!change.invalidate_sql_.empty()) {
        if (OB_FAIL(sql.assign(change.invalidate_sql_))) {
        } else { ret = connection.execute_write(sql, affected); }
      }
      if (OB_SUCC(ret) && affected != 0) {
        if (OB_FAIL(sql.assign(change.cleanup_sql_))) {
        } else if (OB_FAIL(connection.execute_write(sql, affected))) {
        } else if (!change.enqueue_sql_.empty()) {
          if (OB_FAIL(sql.assign(change.enqueue_sql_))) {
          } else { ret = connection.execute_write(sql, affected); }
        }
      }
    }
    const int restore_ret = connection.close();
    if (OB_SUCC(ret)) { ret = restore_ret; }
  }
  changes.reset();
  return ret;
}
} // namespace sql
} // namespace oceanbase
