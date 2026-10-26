/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "ai_column_sql_service.h"
#include "ai_column_schema.h"
#include "ob_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase { namespace share { namespace schema {
using namespace common;

int AIColumnSQLService::delete_current_metadata(ObISQLClient &client, uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column WHERE table_id=%lu", table_id))) {
  } else if (OB_FAIL(client.write(sql.ptr(), affected))) {
  }
  return ret;
}

int AIColumnSQLService::store_metadata(ObISQLClient &client, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  if (!table.has_ai_column_history() || is_inner_table(table.get_table_id())) { return ret; }
  ObArenaAllocator allocator;
  ObString snapshot;
  ObDMLSqlSplicer history;
  ObDMLExecHelper exec(client);
  int64_t affected = 0;
  if (OB_FAIL(table.serialize_ai_column_metadata(allocator, snapshot))) {
  } else if (OB_FAIL(history.add_pk_column("table_id", table.get_table_id()))) {
  } else if (OB_FAIL(history.add_pk_column("schema_version", table.get_schema_version()))) {
  } else if (OB_FAIL(history.add_column("metadata", ObHexEscapeSqlStr(snapshot)))) {
  } else if (OB_FAIL(exec.exec_insert_update("oceanbase.__all_ai_column_history", history, affected))) {
  } else if (OB_FAIL(delete_current_metadata(client, table.get_table_id()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_ai_column_count(); ++i) {
    const AIColumnSchema &column = *table.get_ai_column_by_idx(i);
    ObDMLSqlSplicer dml;
    const int64_t size = column.get_serialize_size();
    char *buffer = static_cast<char *>(allocator.alloc(size));
    int64_t pos = 0;
    if (buffer == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(column.serialize(buffer, size, pos))) {
    } else if (OB_FAIL(dml.add_pk_column("table_id", table.get_table_id()))) {
    } else if (OB_FAIL(dml.add_pk_column("column_id", column.column_id_))) {
    } else if (OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(column.get_column_name())))) {
    } else if (OB_FAIL(dml.add_column("state_column_id", column.state_column_id_))) {
    } else if (OB_FAIL(dml.add_column("request_column_id", column.request_column_id_))) {
    } else if (OB_FAIL(dml.add_column("result_spec_column_id", column.result_spec_column_id_))) {
    } else if (OB_FAIL(dml.add_column("definition_version", column.definition_version_))) {
    } else if (OB_FAIL(dml.add_column("owner_user_id", column.owner_user_id_))) {
    } else if (OB_FAIL(dml.add_column("max_retries", column.max_retries_))) {
    } else if (OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(column.get_normalized_expr())))) {
    } else if (OB_FAIL(dml.add_column("original_expr", ObHexEscapeSqlStr(column.get_original_expr())))) {
    } else if (OB_FAIL(dml.add_column("model_name", ObHexEscapeSqlStr(column.get_model_name())))) {
    } else if (OB_FAIL(dml.add_column("metadata", ObHexEscapeSqlStr(ObString(pos, buffer))))) {
    } else if (OB_FAIL(exec.exec_insert("oceanbase.__all_ai_column", dml, affected))) {
    }
  }
  return ret;
}
} } }
