/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ai_column_worker.h"
#include "share/ai_service/ai_column_sql.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/json_type/ob_json_tree.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/time/ob_time_utility.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "share/object/ob_obj_cast.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace observer
{
namespace
{
struct AiTask
{
  uint64_t column_id_ = OB_INVALID_ID;
  int64_t version_ = 0;
  int64_t state_ = 0;
  int64_t retries_ = 0;
  int64_t max_retries_ = 0;
  bool is_null_ = false;
  ObString predicate_;
  ObString result_;
};

int ai_task_condition(ObSqlString &sql, uint64_t table_id, const ObString &request,
                      const ObString *owner = nullptr)
{
  int ret = sql.append_fmt("table_id=%lu AND request_id=", table_id);
  if (OB_SUCC(ret)) { ret = ai_column_literal(sql, request); }
  if (OB_SUCC(ret) && owner != nullptr) {
    if (OB_FAIL(sql.append(" AND lease_owner="))) {
    } else { ret = ai_column_literal(sql, *owner); }
  }
  return ret;
}

int ai_qualified_table(ObSqlString &sql, const ObTableSchema &table, const ObDatabaseSchema &db)
{
  int ret = ai_column_identifier(sql, db.get_database_name_str());
  if (OB_SUCC(ret) && OB_SUCC(ret = sql.append("."))) {
    ret = ai_column_identifier(sql, table.get_table_name_str());
  }
  return ret;
}

int ai_verify_owner(ObISQLClient &client, const ObSqlString &owned, int64_t expected)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObISQLClient::ReadResult result;
  int64_t count = 0;
  if (OB_FAIL(sql.append("SELECT COUNT(*) AS n FROM oceanbase.__all_ai_column_task WHERE "))) {
  } else if (OB_FAIL(sql.append(owned.string()))) {
  } else if (OB_FAIL(client.read(result, sql.ptr()))) {
  } else if (result.mysql_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(result.mysql_result()->next())) {
  } else if (OB_FAIL(result.mysql_result()->get_int("n", count))) {
  } else if (count != expected) { ret = OB_EAGAIN; }
  return ret;
}

// Validate the complete response before native DML performs the final type cast.
// In particular, MySQL's permissive conversion of "abc" to zero is not success.
int ai_validate_result(ObIAllocator &alloc, const ObColumnSchemaV2 &column,
                       const AiColumnDefinition &definition, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (column.is_json() || definition.vector_dimension_ > 0) {
    ObIJsonBase *json = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&alloc, value, ObJsonInType::JSON_TREE,
                                                ObJsonInType::JSON_TREE, json))) {
    } else if (definition.vector_dimension_ > 0) {
      if (json->json_type() != ObJsonNodeType::J_ARRAY
          || json->element_count() != definition.vector_dimension_) {
        ret = OB_INVALID_ARGUMENT;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < definition.vector_dimension_; ++i) {
        ObIJsonBase *element = nullptr;
        double number = 0;
        if (OB_FAIL(json->get_array_element(i, element))) {
        } else if (!element->is_json_number(element->json_type())) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(element->to_double(number))) {
        } else if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number))) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
    }
  } else if (ob_is_numeric_type(column.get_data_type())) {
    std::string input(value.ptr(), value.length());
    char *end = nullptr;
    errno = 0;
    const double numeric = std::strtod(input.c_str(), &end);
    if (end == input.c_str() || end != input.c_str() + input.size()
        || errno == ERANGE || !std::isfinite(numeric)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (ob_is_integer_type(column.get_data_type())) {
      int64_t i = (value.length() > 0 && (value.ptr()[0] == '-' || value.ptr()[0] == '+')) ? 1 : 0;
      if (i == value.length()) { ret = OB_INVALID_ARGUMENT; }
      for (; OB_SUCC(ret) && i < value.length(); ++i) {
        if (value.ptr()[i] < '0' || value.ptr()[i] > '9') { ret = OB_INVALID_ARGUMENT; }
      }
    }
    if (OB_SUCC(ret)) {
      ObObj source;
      ObObj converted;
      source.set_varchar(value);
      source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      ObAccuracy accuracy(column.get_accuracy());
      ObCastCtx cast(&alloc, nullptr, CM_NONE, column.get_collation_type(), &accuracy);
      ret = ObObjCaster::to_type(column.get_data_type(), column.get_collation_type(), cast,
                                source, converted);
      if (OB_SUCC(ret) && cast.warning_ != OB_SUCCESS) { ret = cast.warning_; }
    }
  } else if (ob_is_string_type(column.get_data_type())) {
    const int64_t length = column.get_length_semantics() == LS_CHAR
        ? ObCharset::strlen_char(column.get_collation_type(), value.ptr(), value.length()) : value.length();
    if (length > column.get_data_length()) { ret = OB_SIZE_OVERFLOW; }
  } else if (ob_is_date_tc(column.get_data_type())) {
    int32_t date = 0;
    ret = ObTimeConverter::str_to_date(value, date, SMO_STRICT_ALL_TABLES | SMO_NO_ZERO_DATE | SMO_NO_ZERO_IN_DATE);
  } else if (ob_is_datetime_tc(column.get_data_type())) {
    int64_t date = 0;
    ObTimeConvertCtx context(nullptr, false);
    ret = ObTimeConverter::str_to_datetime(value, context, date, nullptr,
                                         SMO_STRICT_ALL_TABLES | SMO_NO_ZERO_DATE | SMO_NO_ZERO_IN_DATE);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ai_read_tasks(ObISQLClient &client, ObIAllocator &alloc, const ObSqlString &condition,
                   ObIArray<AiTask> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObISQLClient::ReadResult result;
  if (OB_FAIL(sql.append("SELECT column_id,definition_version,state,retry_count,max_retries,"
                         "rowkey_predicate,result_value FROM oceanbase.__all_ai_column_task WHERE "))) {
  } else if (OB_FAIL(sql.append(condition.string()))) {
  } else if (OB_FAIL(sql.append(" ORDER BY column_id"))) {
  } else if (OB_FAIL(client.read(result, sql.ptr()))) {
  } else if (result.mysql_result() == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    sqlclient::ObMySQLResult &rows = *result.mysql_result();
    while (OB_SUCC(ret) && OB_SUCC(ret = rows.next())) {
      AiTask task;
      int64_t column_id = 0;
      ObString text;
      if (OB_FAIL(rows.get_int("column_id", column_id))) {
      } else if (OB_FAIL(rows.get_int("definition_version", task.version_))) {
      } else if (OB_FAIL(rows.get_int("state", task.state_))) {
      } else if (OB_FAIL(rows.get_int("retry_count", task.retries_))) {
      } else if (OB_FAIL(rows.get_int("max_retries", task.max_retries_))) {
      } else if (OB_FAIL(rows.get_varchar("rowkey_predicate", text))) {
      } else if (OB_FAIL(ob_write_string(alloc, text, task.predicate_))) {
      } else {
        task.column_id_ = column_id;
        if (OB_FAIL(rows.get_varchar("result_value", text))) {
          if (ret == OB_ERR_NULL_VALUE) { task.is_null_ = true; ret = OB_SUCCESS; }
        } else { ret = ob_write_string(alloc, text, task.result_); }
        if (OB_SUCC(ret)) { ret = tasks.push_back(task); }
      }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  return ret;
}
} // namespace

int AiColumnWorker::process_one()
{
  int ret = OB_SUCCESS;
  if (GCTX.sql_proxy_ == nullptr || GCTX.schema_service_ == nullptr
      || !GCTX.schema_service_->is_runtime_schema_refreshed()) { return OB_EAGAIN; }
  ObArenaAllocator alloc("AiColumnWorker");
  ObString request;
  uint64_t table_id = OB_INVALID_ID;
  {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(GCTX.sql_proxy_->read(result,
        "SELECT table_id,request_id FROM oceanbase.__all_ai_column_task "
        "WHERE (lease_expiry IS NULL OR lease_expiry < NOW(6)) "
        "AND (eligible_ts IS NULL OR eligible_ts <= NOW(6)) "
        "ORDER BY eligible_ts,table_id,column_id LIMIT 1"))) {
    } else if (result.mysql_result() == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result.mysql_result()->next())) {
    } else {
      int64_t id = 0;
      ObString text;
      if (OB_FAIL(result.mysql_result()->get_int("table_id", id))) {
      } else if (OB_FAIL(result.mysql_result()->get_varchar("request_id", text))) {
      } else if (OB_FAIL(ob_write_string(alloc, text, request))) {
      } else { table_id = id; }
    }
  }
  char uuid[sql::UuidCommon::LENGTH_UUID + 1] = {};
  ObString owner;
  ObSqlString group;
  ObSqlString owned;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_SUCC(ret)) {
    if (OB_FAIL(sql::ObExprUuid::gen_server_uuid(uuid, sql::UuidCommon::LENGTH_UUID))) {
    } else {
      owner.assign_ptr(uuid, sql::UuidCommon::LENGTH_UUID);
      if (OB_FAIL(ai_task_condition(group, table_id, request))) {
      } else if (OB_FAIL(ai_task_condition(owned, table_id, request, &owner))) {
      } else if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET lease_owner="))) {
      } else if (OB_FAIL(ai_column_literal(sql, owner))) {
      } else if (OB_FAIL(sql.append(",lease_expiry=DATE_ADD(NOW(6),INTERVAL 300 SECOND) WHERE "))) {
      } else if (OB_FAIL(sql.append(group.string()))) {
      } else if (OB_FAIL(sql.append(" AND (lease_expiry IS NULL OR lease_expiry < NOW(6))"))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
      } else if (affected == 0) { ret = OB_EAGAIN; }
    }
  }
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSEArray<AiTask, 4> tasks;
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ai_read_tasks(*GCTX.sql_proxy_, alloc, owned, tasks))) {
    } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
    } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table))) {
    } else if (table != nullptr && OB_FAIL(schema_guard.get_database_schema(table->get_database_id(), database))) {
    }
  }
  bool obsolete = table == nullptr || database == nullptr;
  if (OB_SUCC(ret) && !tasks.empty()) { LOG_DEBUG("AI column claimed group", K(table_id), K(request), K(tasks.count()), K(obsolete)); }
  for (int64_t i = 0; OB_SUCC(ret) && !obsolete && i < tasks.count(); ++i) {
    AiTask &task = tasks.at(i);
    const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id_);
    AiColumnDefinition definition;
    if (column == nullptr || !column->is_ai_column()) { obsolete = true; LOG_DEBUG("AI column missing definition", K(table_id), K(task.column_id_)); }
    else if (OB_FAIL(column->get_ai_column_definition(definition))) {
    } else if (definition.definition_version_ != task.version_) { obsolete = true; LOG_DEBUG("AI column changed definition", K(table_id), K(task.version_), K(definition.definition_version_)); }
    else if (task.state_ != 2 && task.state_ != 4) {
      sql.reset();
      // Renew the whole group before a potentially slow model request.
      if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET lease_expiry=DATE_ADD(NOW(6),INTERVAL 300 SECOND) WHERE "))) {
      } else if (OB_FAIL(sql.append(owned.string()))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
      } else if (OB_FAIL(ai_verify_owner(*GCTX.sql_proxy_, owned, tasks.count()))) {
      }
      int call_ret = OB_SUCCESS;
      ObString output;
      bool is_null = false;
      sql.reset();
      if (OB_SUCC(ret) && !obsolete) {
        if (OB_FAIL(sql.append("SELECT CAST(("))) {
        } else if (OB_FAIL(sql.append(definition.normalized_expression_))) {
        } else if (OB_FAIL(sql.append(") AS CHAR) AS ai_result FROM "))) {
        } else if (OB_FAIL(ai_qualified_table(sql, *table, *database))) {
        } else if (OB_FAIL(sql.append(" WHERE "))) {
        } else if (OB_FAIL(sql.append(task.predicate_))) {
        } else if (OB_FAIL(sql.append(" AND "))) {
        } else if (OB_FAIL(ai_column_identifier(sql, table->get_column_schema(definition.request_column_id_)->get_column_name_str()))) {
        } else if (OB_FAIL(sql.append("="))) {
        } else if (OB_FAIL(ai_column_literal(sql, request))) {
        } else if (OB_FAIL(sql.append(" AND "))) {
        } else if (OB_FAIL(ai_column_identifier(sql, table->get_column_schema(definition.result_spec_column_id_)->get_column_name_str()))) {
        } else if (OB_FAIL(sql.append_fmt("=%ld", task.version_))) {
        } else {
          AiColumnExecutionGuard execution(&definition.type_prompt_);
          ObISQLClient::ReadResult result;
          if (OB_SUCCESS != (call_ret = GCTX.sql_proxy_->read(result, sql.ptr()))) {
          } else if (result.mysql_result() == nullptr) {
            call_ret = OB_ERR_UNEXPECTED;
          } else if (OB_SUCCESS != (call_ret = result.mysql_result()->next())) {
            if (call_ret == OB_ITER_END) { obsolete = true; call_ret = OB_SUCCESS; LOG_DEBUG("AI column obsolete row", K(sql)); }
          } else {
            ObString value;
            if (OB_SUCCESS != (call_ret = result.mysql_result()->get_varchar("ai_result", value))) {
              if (call_ret == OB_ERR_NULL_VALUE) { is_null = true; call_ret = OB_SUCCESS; }
            } else if (OB_SUCCESS != (call_ret = ai_validate_result(alloc, *column, definition, value))) {
            } else { call_ret = ob_write_string(alloc, value, output); }
          }
        }
      }
      if (OB_SUCC(ret) && !obsolete) {
        if (call_ret != OB_SUCCESS) { LOG_INFO("AI column model evaluation failed", K(call_ret), K(sql), K(task.column_id_)); }
        task.state_ = call_ret == OB_SUCCESS ? 2 : ++task.retries_ <= task.max_retries_ ? 3 : 4;
        task.result_ = output;
        task.is_null_ = is_null || call_ret != OB_SUCCESS;
        sql.reset();
        if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%ld,retry_count=%ld,last_error=%d,result_value=",
                                   task.state_, task.retries_, call_ret))) {
        } else if (task.is_null_) { ret = sql.append("NULL"); }
        else { ret = ai_column_literal(sql, output); }
        if (OB_SUCC(ret) && OB_SUCC(ret = sql.append(",eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND) WHERE "))
            && OB_SUCC(ret = sql.append(owned.string()))
            && OB_SUCC(ret = sql.append_fmt(" AND column_id=%lu", task.column_id_))) {
          ret = GCTX.sql_proxy_->write(sql.ptr(), affected);
          if (OB_SUCC(ret) && affected == 0) { obsolete = true; }
        }
      }
    }
  }
  bool terminal = !tasks.empty();
  for (int64_t i = 0; i < tasks.count(); ++i) {
    terminal &= tasks.at(i).state_ == 2 || tasks.at(i).state_ == 4;
  }
  if (OB_SUCC(ret) && (terminal || obsolete)) {
    // A model call may outlive an ALTER or DROP. Refresh schema before building
    // publication SQL; row request/spec predicates provide the final write fence.
    ObSchemaGetterGuard publish_guard;
    if (!obsolete) {
      if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(publish_guard))) {
      } else if (OB_FAIL(publish_guard.get_table_schema(table_id, table))) {
      } else if (table == nullptr) { obsolete = true; }
      else if (OB_FAIL(publish_guard.get_database_schema(table->get_database_id(), database))) {
      } else if (database == nullptr) { obsolete = true; }
      for (int64_t i = 0; OB_SUCC(ret) && !obsolete && i < tasks.count(); ++i) {
        const ObColumnSchemaV2 *current = table->get_column_schema(tasks.at(i).column_id_);
        AiColumnDefinition definition;
        if (current == nullptr || !current->is_ai_column()) { obsolete = true; }
        else if (OB_FAIL(current->get_ai_column_definition(definition))) {
        } else if (definition.definition_version_ != tasks.at(i).version_
                   || table->get_column_schema(definition.state_column_id_) == nullptr
                   || table->get_column_schema(definition.request_column_id_) == nullptr
                   || table->get_column_schema(definition.result_spec_column_id_) == nullptr) {
          obsolete = true;
        }
      }
    }
    ObMySQLTransaction transaction;
    AiColumnExecutionGuard execution;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(transaction.start(GCTX.sql_proxy_))) {
    } else {
      // Lock and verify ownership before publishing. DML and publication use the
      // same database transaction mechanism, including native vector indexes.
      sql.reset();
      if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET lease_expiry=DATE_ADD(NOW(6),INTERVAL 300 SECOND) WHERE "))) {
      } else if (OB_FAIL(sql.append(owned.string()))) {
      } else if (OB_FAIL(transaction.write(sql.ptr(), affected))) {
      } else if (OB_FAIL(ai_verify_owner(transaction, owned, tasks.count()))) {
      }
      if (OB_SUCC(ret) && !obsolete) {
        sql.reset();
        if (OB_FAIL(sql.append("UPDATE "))) {
        } else if (OB_FAIL(ai_qualified_table(sql, *table, *database))) {
        } else { ret = sql.append(" SET "); }
        for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
          const AiTask &task = tasks.at(i);
          const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id_);
          AiColumnDefinition definition;
          if (OB_FAIL(column->get_ai_column_definition(definition))) {
          } else if (i > 0 && OB_FAIL(sql.append(","))) {
          } else if (OB_FAIL(ai_column_identifier(sql, column->get_column_name_str()))) {
          } else if (OB_FAIL(sql.append("="))) {
          } else if (task.state_ == 4 || task.is_null_) { ret = sql.append("NULL"); }
          else {
            if (OB_FAIL(sql.append("CONVERT("))) {
            } else if (OB_FAIL(ai_column_literal(sql, task.result_))) {
            } else { ret = sql.append(" USING utf8mb4)"); }
          }
          if (OB_SUCC(ret) && OB_SUCC(ret = sql.append(","))
              && OB_SUCC(ret = ai_column_identifier(sql, table->get_column_schema(definition.state_column_id_)->get_column_name_str()))) {
            ret = sql.append_fmt("=%d", task.state_ == 2 ? 1 : 2);
          }
        }
        if (OB_SUCC(ret) && OB_SUCC(ret = sql.append(" WHERE "))) { ret = sql.append(tasks.at(0).predicate_); }
        for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
          AiColumnDefinition definition;
          if (OB_FAIL(table->get_column_schema(tasks.at(i).column_id_)->get_ai_column_definition(definition))) {
          } else if (OB_FAIL(sql.append(" AND "))) {
          } else if (OB_FAIL(ai_column_identifier(sql, table->get_column_schema(definition.request_column_id_)->get_column_name_str()))) {
          } else if (OB_FAIL(sql.append("="))) {
          } else if (OB_FAIL(ai_column_literal(sql, request))) {
          } else if (OB_FAIL(sql.append(" AND "))) {
          } else if (OB_FAIL(ai_column_identifier(sql, table->get_column_schema(definition.result_spec_column_id_)->get_column_name_str()))) {
          } else { ret = sql.append_fmt("=%ld", tasks.at(i).version_); }
        }
        if (OB_SUCC(ret)) { ret = transaction.write(sql.ptr(), affected); }
      }
      sql.reset();
      if (OB_SUCC(ret) && OB_SUCC(ret = sql.append("DELETE FROM oceanbase.__all_ai_column_task WHERE "))
          && OB_SUCC(ret = sql.append(owned.string()))) { ret = transaction.write(sql.ptr(), affected); }
      const int end_ret = transaction.end(OB_SUCC(ret));
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  // Retryable groups release the lease, preserving successful staged columns.
  // Infrastructure errors do the same; a crashed owner is recovered by expiry.
  if (!owned.empty()) {
    sql.reset();
    int release_ret = sql.append("UPDATE oceanbase.__all_ai_column_task SET lease_owner=NULL,lease_expiry=NULL,eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND) WHERE ");
    if (OB_SUCC(release_ret)) { release_ret = sql.append(owned.string()); }
    if (OB_SUCC(release_ret)) { release_ret = GCTX.sql_proxy_->write(sql.ptr(), affected); }
    if (OB_SUCC(ret)) { ret = release_ret; }
  }
  return ret;
}

int AiColumnWorker::enqueue_missing()
{
  int ret = OB_SUCCESS;
  if (GCTX.sql_proxy_ == nullptr || GCTX.schema_service_ == nullptr
      || !GCTX.schema_service_->is_runtime_schema_refreshed()) { return OB_EAGAIN; }
  ObSEArray<AiColumnReference, 8> columns;
  {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(GCTX.sql_proxy_->read(result,
        "SELECT table_id,column_id FROM oceanbase.__all_ai_column ORDER BY table_id,column_id"))) {
    } else if (result.mysql_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      while (OB_SUCC(ret) && OB_SUCC(ret = result.mysql_result()->next())) {
        int64_t table_id = 0;
        int64_t column_id = 0;
        if (OB_FAIL(result.mysql_result()->get_int("table_id", table_id))) {
        } else if (OB_FAIL(result.mysql_result()->get_int("column_id", column_id))) {
        } else { ret = columns.push_back(AiColumnReference(table_id, column_id)); }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    }
  }
  ObSchemaGetterGuard guard;
  if (OB_SUCC(ret)) { ret = GCTX.schema_service_->get_runtime_schema_guard(guard); }
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < columns.count(); ++i) {
    const AiColumnReference &ref = columns.at(i);
    const ObTableSchema *table = nullptr;
    const ObDatabaseSchema *database = nullptr;
    const ObColumnSchemaV2 *column = nullptr;
    AiColumnDefinition definition;
    if (OB_FAIL(guard.get_table_schema(ref.table_id_, table))) {
    } else if (table == nullptr || (column = table->get_column_schema(ref.column_id_)) == nullptr) {
      continue;
    } else if (OB_FAIL(column->get_ai_column_definition(definition))) {
    } else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
    } else if (database != nullptr && table->get_rowkey_info().get_size() > 0) {
      const ObColumnSchemaV2 *spec = table->get_column_schema(definition.result_spec_column_id_);
      const ObColumnSchemaV2 *key = table->get_column_schema(table->get_rowkey_info().get_column(0)->column_id_);
      ObSqlString sql;
      if (spec == nullptr || key == nullptr) { ret = OB_SCHEMA_EAGAIN; }
      else if (OB_FAIL(sql.append("UPDATE "))) {
      } else if (OB_FAIL(ai_qualified_table(sql, *table, *database))) {
      } else if (OB_FAIL(sql.append(" SET "))) {
      } else if (OB_FAIL(ai_column_identifier(sql, key->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append("="))) {
      } else if (OB_FAIL(ai_column_identifier(sql, key->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append(" WHERE "))) {
      } else if (OB_FAIL(ai_column_identifier(sql, spec->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append(" IS NULL OR "))) {
      } else if (OB_FAIL(ai_column_identifier(sql, spec->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append_fmt("<>%ld LIMIT 64", definition.definition_version_))) {
      } else {
        // Native DML creates tasks and invalidates results in its transaction.
        // Rechecking spec under row locks prevents duplicate backfill requests.
        AiColumnRetryGuard retry(ref.table_id_, ref.column_id_);
        int64_t affected = 0;
        ret = GCTX.sql_proxy_->write(sql.ptr(), affected);
      }
    }
  }
  return ret;
}

void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumnWorker");
  int64_t last_backfill = 0;
  while (!has_set_stop()) {
    const int64_t now = ObTimeUtility::current_time();
    if (now - last_backfill >= 5000000) {
      last_backfill = now;
      const int ret = enqueue_missing();
      if (ret != OB_SUCCESS && ret != OB_EAGAIN && REACH_TIME_INTERVAL(10000000)) {
        LOG_WARN("AI column backfill will retry", K(ret));
      }
    }
    const int ret = process_one();
    if (ret != OB_SUCCESS) {
      if (ret != OB_ITER_END && ret != OB_EAGAIN && REACH_TIME_INTERVAL(10000000)) {
        LOG_WARN("AI column worker will retry", K(ret));
      }
      usleep(100000);
    }
  }
}
} // namespace observer
} // namespace oceanbase
