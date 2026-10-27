/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER_OMT
#include "ai_column_worker.h"
#include "observer/ob_server.h"
#include "observer/ob_inner_sql_result.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/engine/expr/ob_expr_column_conv.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/thread/ob_thread_name.h"
#include <cmath>

namespace oceanbase { namespace omt {
using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace share::schema;
namespace {
constexpr int64_t INPUT_LIMIT = 1024 * 1024;
constexpr int64_t OUTPUT_LIMIT = 4 * 1024 * 1024;
constexpr int64_t LEASE_SECONDS = 90;

int identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = name.ptr()[i] == '`' ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int hex_literal(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    ret = sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i]));
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}

int table_name(ObSchemaGetterGuard &guard, const ObTableSchema &table, ObSqlString &sql, ObString &database_name)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {
  } else if (database == nullptr) { ret = OB_SCHEMA_EAGAIN;
  } else if (OB_FAIL(identifier(sql, database->get_database_name_str()))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(identifier(sql, table.get_table_name_str()))) {
  } else { database_name = database->get_database_name_str(); }
  return ret;
}

// Called while this result still owns its snapshot and LOB context. The copy
// survives closing the result, which always precedes any HTTP request.
int read_text(ObMySQLResult &result, const char *name, ObIAllocator &allocator,
    ObString &text, int64_t limit)
{
  int ret = OB_SUCCESS;
  ObObj value;
  auto *native = dynamic_cast<observer::ObInnerSQLResult *>(&result);
  ObString real;
  if (native == nullptr) { ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(result.get_obj(name, value))) {
  } else if (value.is_null()) { ret = OB_ERR_NULL_VALUE;
  } else if (OB_FAIL(sql::ObTextStringHelper::read_real_string_data(
          native->result_set().get_exec_context(), &allocator, value, real))) {
  } else if (real.length() > limit) { ret = OB_SIZE_OVERFLOW;
  } else { ret = ob_write_string(allocator, real, text); }
  return ret;
}

int check_owner(ObSchemaGetterGuard &guard, const ObTableSchema &table,
    const AIColumnSchema &ai, const ObString &database, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSessionPrivInfo session;
  ObStmtNeedPrivs needed(allocator);
  ObSEArray<uint64_t, 4> roles;
  bool model_access = false;
  sql::ObAIServiceEndpointPrivUtil model_privileges(guard);
  ObNeedPriv privilege(database, table.get_table_name_str(), OB_PRIV_TABLE_LEVEL,
      OB_PRIV_SELECT | OB_PRIV_UPDATE, false);
  needed.need_privs_.set_capacity(1);
  if (OB_FAIL(guard.get_session_priv_info(ai.owner_user_id_, database, session))) {
  } else if (OB_FAIL(needed.need_privs_.push_back(privilege))) {
  } else if (OB_FAIL(guard.check_priv(session, roles, needed))) {
  } else if (OB_FAIL(model_privileges.check_access_ai_model_priv(allocator, session, model_access))) {
  } else if (!model_access) { ret = OB_ERR_NO_PRIVILEGE; }
  return ret;
}

int validate_output(ObIAllocator &allocator, const ObColumnSchemaV2 &column,
    const AIColumnSchema &ai, const ObString &output)
{
  int ret = OB_SUCCESS;
  if (output.length() > OUTPUT_LIMIT) { ret = OB_SIZE_OVERFLOW;
  } else if (ai.embedding_) {
    ObIJsonBase *json = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, output,
            ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json))) {
    } else if (json == nullptr || json->json_type() != ObJsonNodeType::J_ARRAY
        || json->element_count() != ai.vector_dimension_) { ret = OB_INVALID_DATA;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < json->element_count(); ++i) {
        double value = 0;
        ObJsonNode *element = static_cast<ObJsonArray *>(json)->get_value(i);
        if (element == nullptr || OB_FAIL(element->to_double(value))) {
          ret = OB_INVALID_DATA;
        } else if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value))) {
          ret = OB_INVALID_DATA;
        }
      }
    }
  } else {
    ObObj source, converted;
    source.set_varchar(output);
    source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    sql::ObExprResType type;
    type.set_type(column.get_data_type());
    type.set_collation_type(column.get_collation_type());
    type.set_accuracy(column.get_accuracy());
    ObCastCtx context(&allocator, nullptr, CM_STRICT_MODE | CM_COLUMN_CONVERT
        | CM_ERROR_ON_SCALE_OVER | CM_NO_ZERO_DATE | CM_NO_ZERO_IN_DATE, column.get_collation_type());
    ret = sql::ObExprColumnConv::convert_skip_null_check(converted, source, type, true, context);
  }
  return ret;
}
} // namespace

struct AIColumnWorker::Group {
  uint64_t table_id_ = OB_INVALID_ID;
  int64_t epoch_ = 0;
  ObString request_;
  ObString predicate_;
};
struct AIColumnWorker::Task {
  uint64_t column_id_ = OB_INVALID_ID;
  int64_t definition_ = 0;
  int64_t state_ = 0;
  int64_t retries_ = 0;
  bool eligible_ = true;
};

AIColumnWorker::AIColumnWorker() : sql_(nullptr), cancellation_(false), reclaimed_(0) {}
AIColumnWorker::~AIColumnWorker() { stop(); wait(); }

int AIColumnWorker::start()
{
  sql_ = observer::ObServer::get_instance().get_gctx().sql_proxy_;
  if (sql_ == nullptr) { return OB_NOT_INIT; }
  cancellation_.store(false);
  reclaimed_.store(0);
  snprintf(prefix_buffer_, sizeof(prefix_buffer_), "%lx:", observer::ObServer::get_instance().get_self().hash());
  snprintf(owner_buffer_, sizeof(owner_buffer_), "%s%ld:%d", prefix_buffer_, ObTimeUtility::current_time(), getpid());
  prefix_ = ObString::make_string(prefix_buffer_);
  owner_ = ObString::make_string(owner_buffer_);
  int ret = set_thread_count(2);
  if (OB_SUCC(ret)) { ret = share::ObThreadPool::start(); }
  return ret;
}
void AIColumnWorker::stop() { cancellation_.store(true); share::ObThreadPool::stop(); }

void AIColumnWorker::run1()
{
  lib::set_thread_name("AIColumn");
  while (!has_set_stop()) {
    if (sql_ != nullptr) {
      int expected = 0;
      if (reclaimed_.compare_exchange_strong(expected, 1)) {
        reclaimed_.store(reclaim_previous_boot() == OB_SUCCESS ? 2 : 0);
      }
      if (reclaimed_.load() == 2) { (void) process_one(); }
    }
    ob_usleep(100 * 1000);
  }
}

int AIColumnWorker::reclaim_previous_boot()
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  // A seekdb instance has exclusive ownership of its local store/address.
  // Reclaim this address's prior boot immediately; other instances use leases.
  if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_group SET lease_owner='',lease_until=NULL "
          "WHERE lease_owner LIKE '%.*s%%'", prefix_.length(), prefix_.ptr()))) {
  } else { ret = sql_->write(sql.ptr(), affected); }
  return ret;
}

int AIColumnWorker::process_one()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  Group group;
  {
    SMART_VAR(ObMySQLProxy::MySQLResult, result) {
      ObMySQLResult *row = nullptr;
      if (OB_FAIL(sql_->read(result, "SELECT table_id,request_id,rowkey_predicate,lease_epoch "
              "FROM oceanbase.__all_ai_column_group WHERE eligible_ts<=NOW(6) AND state=0 "
              "AND (lease_until IS NULL OR lease_until<=NOW(6)) "
              "ORDER BY eligible_ts,table_id,request_id LIMIT 1"))) {
      } else if ((row = result.get_result()) == nullptr) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(row->next())) {
      } else if (OB_FAIL(row->get_uint("table_id", group.table_id_))) {
      } else if (OB_FAIL(row->get_int("lease_epoch", group.epoch_))) {
      } else if (OB_FAIL(read_text(*row, "request_id", allocator, group.request_, 64))) {
      } else { ret = read_text(*row, "rowkey_predicate", allocator, group.predicate_, 64 * 1024); }
    }
  }
  if (OB_SUCC(ret)) {
    ObSqlString claim;
    int64_t affected = 0;
    if (OB_FAIL(claim.append("UPDATE oceanbase.__all_ai_column_group SET lease_owner="))) {
    } else if (OB_FAIL(hex_literal(claim, owner_))) {
    } else if (OB_FAIL(claim.append_fmt(",lease_epoch=lease_epoch+1,lease_until=TIMESTAMPADD(SECOND,%ld,NOW(6)) "
            "WHERE table_id=%lu AND request_id=", LEASE_SECONDS, group.table_id_))) {
    } else if (OB_FAIL(hex_literal(claim, group.request_))) {
    } else if (OB_FAIL(claim.append_fmt(" AND lease_epoch=%ld AND (lease_until IS NULL OR lease_until<=NOW(6))", group.epoch_))) {
    } else if (OB_FAIL(sql_->write(claim.ptr(), affected))) {
    } else if (affected != 1) { ret = OB_EAGAIN;
    } else {
      ++group.epoch_;
      ret = process_group(group);
      if (ret != OB_SUCCESS && !cancellation_.load()) { (void) release_group(group); }
    }
  }
  return ret;
}

int AIColumnWorker::release_group(const Group &group)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_group SET lease_owner='',lease_until=NULL,"
          "eligible_ts=COALESCE((SELECT MIN(eligible_ts) FROM oceanbase.__all_ai_column_task "
          "WHERE table_id=%lu AND request_id=", group.table_id_))) {
  } else if (OB_FAIL(hex_literal(sql, group.request_))) {
  } else if (OB_FAIL(sql.append_fmt(" AND state IN(0,3)),NOW(6)) WHERE table_id=%lu AND request_id=", group.table_id_))) {
  } else if (OB_FAIL(hex_literal(sql, group.request_))) {
  } else if (OB_FAIL(sql.append_fmt(" AND lease_epoch=%ld AND lease_owner=", group.epoch_))) {
  } else if (OB_FAIL(hex_literal(sql, owner_))) {
  } else { ret = sql_->write(sql.ptr(), affected); }
  return ret;
}

int AIColumnWorker::save_outcome(const Group &group, const Task &task, int error,
    const ObString &outcome, int64_t max_retries)
{
  int ret = OB_SUCCESS;
  const bool retry = error != OB_SUCCESS && task.retries_ < max_retries;
  const int64_t retries = task.retries_ + (retry ? 1 : 0);
  const int64_t delay = retry ? (1LL << std::min<int64_t>(retries - 1, 5)) : 0;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d,retry_count=%ld,error_code=%d,"
          "eligible_ts=TIMESTAMPADD(SECOND,%ld,NOW(6)),outcome=", error == OB_SUCCESS ? 1 : (retry ? 3 : 2), retries, error, delay))) {
  } else if (error == OB_SUCCESS ? OB_FAIL(hex_literal(sql, outcome)) : OB_FAIL(sql.append("NULL"))) {
  } else if (OB_FAIL(sql.append_fmt(" WHERE table_id=%lu AND column_id=%lu AND definition_version=%ld AND request_id=",
          group.table_id_, task.column_id_, task.definition_))) {
  } else if (OB_FAIL(hex_literal(sql, group.request_))) {
  } else if (OB_FAIL(sql.append_fmt(" AND EXISTS(SELECT 1 FROM oceanbase.__all_ai_column_group WHERE table_id=%lu AND request_id=", group.table_id_))) {
  } else if (OB_FAIL(hex_literal(sql, group.request_))) {
  } else if (OB_FAIL(sql.append_fmt(" AND lease_epoch=%ld AND lease_owner=", group.epoch_))) {
  } else if (OB_FAIL(hex_literal(sql, owner_))) {
  } else if (OB_FAIL(sql.append(")"))) {
  } else if (OB_FAIL(sql_->write(sql.ptr(), affected))) {
  } else if (affected != 1) { ret = OB_EAGAIN; }
  return ret;
}

int AIColumnWorker::process_group(const Group &group)
{
  int ret = OB_SUCCESS;
  ObSEArray<Task, 4> tasks;
  ObSqlString query;
  if (OB_FAIL(query.append_fmt("SELECT column_id,definition_version,state,retry_count,eligible_ts<=NOW(6) AS eligible "
          "FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND request_id=", group.table_id_))) {
  } else if (OB_FAIL(hex_literal(query, group.request_))) {
  } else { ret = query.append(" ORDER BY column_id"); }
  {
    SMART_VAR(ObMySQLProxy::MySQLResult, result) {
      ObMySQLResult *row = nullptr;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sql_->read(result, query.ptr()))) {
      } else if ((row = result.get_result()) == nullptr) { ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(ret = row->next())) {
          Task task;
          if (tasks.count() >= 4096) { ret = OB_SIZE_OVERFLOW;
          } else if (OB_FAIL(row->get_uint("column_id", task.column_id_))) {
          } else if (OB_FAIL(row->get_int("definition_version", task.definition_))) {
          } else if (OB_FAIL(row->get_int("state", task.state_))) {
          } else if (OB_FAIL(row->get_int("retry_count", task.retries_))) {
          } else if (OB_FAIL(row->get_bool("eligible", task.eligible_))) {
          } else { ret = tasks.push_back(task); }
        }
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count() && !cancellation_.load(); ++i) {
    if ((tasks.at(i).state_ == 0 || tasks.at(i).state_ == 3) && tasks.at(i).eligible_) {
      ObSqlString renew;
      int64_t affected = 0;
      if (OB_FAIL(renew.append_fmt("UPDATE oceanbase.__all_ai_column_group SET lease_until=TIMESTAMPADD(SECOND,%ld,NOW(6)) "
              "WHERE table_id=%lu AND request_id=", LEASE_SECONDS, group.table_id_))) {
      } else if (OB_FAIL(hex_literal(renew, group.request_))) {
      } else if (OB_FAIL(renew.append_fmt(" AND lease_epoch=%ld AND lease_owner=", group.epoch_))) {
      } else if (OB_FAIL(hex_literal(renew, owner_))) {
      } else if (OB_FAIL(sql_->write(renew.ptr(), affected))) {
      } else if (affected != 1) { ret = OB_EAGAIN;
      } else { ret = execute_task(group, tasks.at(i)); }
    }
  }
  if (OB_SUCC(ret) && !cancellation_.load()) { ret = finish_group(group); }
  return ret;
}

int AIColumnWorker::execute_task(const Group &group, const Task &task)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const AIColumnSchema *ai = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  ObString database, input, config_text, outcome;
  ObSqlString qualified, query;
  ObObjMeta input_type;
  int64_t max_retries = 0;
  auto *schema_service = observer::ObServer::get_instance().get_gctx().schema_service_;
  if (schema_service == nullptr) { ret = OB_NOT_INIT;
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(group.table_id_, table))) {
  } else if (table == nullptr || (ai = table->get_ai_column(task.column_id_)) == nullptr
      || ai->definition_version_ != task.definition_
      || (column = table->get_column_schema(task.column_id_)) == nullptr) {
    ret = OB_SCHEMA_EAGAIN;
  } else {
    max_retries = ai->max_retries_;
    if (OB_FAIL(table_name(guard, *table, qualified, database))) {
    } else if (OB_FAIL(check_owner(guard, *table, *ai, database, allocator))) {
    }
  }
  if (OB_SUCC(ret)) {
    const ObString &source = ai->get_input_expr();
    const ObString &config = ai->get_config_expr();
    const ObColumnSchemaV2 *request = table->get_column_schema(ai->request_column_id_);
    const ObColumnSchemaV2 *spec = table->get_column_schema(ai->result_spec_column_id_);
    if (request == nullptr || spec == nullptr) { ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(query.append_fmt("SELECT (%.*s) AS input_kind,"
            "OCTET_LENGTH(CONVERT((%.*s) USING utf8mb4)) AS input_bytes,"
            "CONVERT((%.*s) USING utf8mb4) AS input_text,"
            "OCTET_LENGTH(CONVERT((%.*s) USING utf8mb4)) AS config_bytes,"
            "CONVERT((%.*s) USING utf8mb4) AS config_text FROM %s WHERE %.*s AND ",
            source.length(), source.ptr(), source.length(), source.ptr(), source.length(), source.ptr(),
            config.empty() ? 4 : config.length(), config.empty() ? "NULL" : config.ptr(),
            config.empty() ? 4 : config.length(), config.empty() ? "NULL" : config.ptr(),
            qualified.ptr(), group.predicate_.length(), group.predicate_.ptr()))) {
    } else if (OB_FAIL(identifier(query, request->get_column_name_str()))) {
    } else if (OB_FAIL(query.append("="))) {
    } else if (OB_FAIL(hex_literal(query, group.request_))) {
    } else if (OB_FAIL(query.append(" AND "))) {
    } else if (OB_FAIL(identifier(query, spec->get_column_name_str()))) {
    } else { ret = query.append_fmt("='%ld'", task.definition_); }
  }
  {
    SMART_VAR(ObMySQLProxy::MySQLResult, result) {
      ObMySQLResult *row = nullptr;
      int64_t input_bytes = 0, config_bytes = 0;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sql_->read(result, query.ptr()))) {
      } else if ((row = result.get_result()) == nullptr) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(row->next())) {
        if (ret == OB_ITER_END) { max_retries = 0; }
      } else if (OB_FAIL(row->get_type("input_kind", input_type))) {
      } else if (OB_FAIL(row->get_int("input_bytes", input_bytes))) {
      } else if (input_bytes > INPUT_LIMIT) { ret = OB_SIZE_OVERFLOW;
      } else if (OB_FAIL(read_text(*row, "input_text", allocator, input, INPUT_LIMIT))) {
      } else {
        ret = row->get_int("config_bytes", config_bytes);
        if (ret == OB_ERR_NULL_VALUE) { ret = OB_SUCCESS;
        } else if (OB_SUCC(ret) && config_bytes > INPUT_LIMIT) { ret = OB_SIZE_OVERFLOW;
        } else if (OB_SUCC(ret)) { ret = read_text(*row, "config_text", allocator, config_text, INPUT_LIMIT); }
      }
    }
  }
  // All SQL results and transaction snapshots above are closed before inference.
  ObJsonObject *config = nullptr;
  if (OB_SUCC(ret)) {
    ObIJsonBase *parsed = nullptr;
    if (input.empty()) { ret = OB_INVALID_ARGUMENT;
    } else if (input_type.is_json()) {
      if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, input,
              ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, parsed))) {
      } else if (parsed == nullptr || parsed->json_type() != ObJsonNodeType::J_OBJECT
          || !ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(parsed))) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
            allocator, static_cast<ObJsonObject *>(parsed), input);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (!config_text.empty()) {
      parsed = nullptr;
      if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, config_text,
              ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, parsed))) {
      } else if (parsed == nullptr || parsed->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_ARGUMENT;
      } else {
        ObJsonObject *supplied = static_cast<ObJsonObject *>(parsed);
        for (int64_t i = 0; OB_SUCC(ret) && i < supplied->element_count(); ++i) {
          ObString key;
          if (OB_FAIL(supplied->get_key(i, key))) {
          } else if (key == "model" || key == "messages" || key == "input" || key == "prompt" || key == "system") {
            ret = OB_INVALID_ARGUMENT;
          } else if (key != "system_prompt") { ret = config->add(key, supplied->get_value(i)); }
        }
        if (OB_SUCC(ret) && supplied->get_value("system_prompt") != nullptr) {
          ObJsonNode *system = supplied->get_value("system_prompt");
          if (system->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_ARGUMENT;
          } else {
            ObSqlString combined;
            ObJsonString *value = nullptr;
            ObString prompt;
            if (OB_FAIL(combined.append(static_cast<ObJsonString *>(system)->get_str()))) {
            } else if (OB_FAIL(combined.append(ai->get_system_prompt()))) {
            } else if (OB_FAIL(ob_write_string(allocator, combined.string(), prompt))) {
            } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, prompt, value))) {
            } else { ret = config->add("system_prompt", value); }
          }
        }
      }
    }
    if (OB_SUCC(ret) && ai->embedding_) {
      ObJsonInt *dimension = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, ai->vector_dimension_, dimension))) {
      } else { ret = config->add("dimensions", dimension); }
    } else if (OB_SUCC(ret) && config->get_value("system_prompt") == nullptr) {
      ObJsonString *system = nullptr;
      ObString prompt = ai->get_system_prompt();
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, prompt, system))) {
      } else { ret = config->add("system_prompt", system); }
    }
  }
  if (OB_SUCC(ret) && !cancellation_.load()) {
    ObAIFuncExprInfo *info = nullptr;
    ObAiModelEndpointInfo endpoint;
    auto *resolver = ::oceanbase::share::server_service<query::ObIAiEndpointResolver>();
    if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, ai->get_model_name(), guard, info))) {
    } else if (resolver == nullptr || info == nullptr) { ret = OB_NOT_INIT;
    } else if (OB_FAIL(resolver->resolve_by_model_name(ai->get_model_name(), allocator, endpoint, false))) {
    } else {
      ObAIFuncModel model(allocator, *info, endpoint);
      if (ai->embedding_) { ret = model.call_dense_embedding(input, config, outcome, 0, &cancellation_); }
      else { ret = model.call_completion(input, config, outcome, 0, &cancellation_); }
      if (OB_SUCC(ret)) { ret = validate_output(allocator, *column, *ai, outcome); }
    }
  }
  if (cancellation_.load()) { return OB_CANCELED; }
  return save_outcome(group, task, ret, outcome, max_retries);
}

int AIColumnWorker::finish_group(const Group &group)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  ObString database;
  ObSqlString qualified;
  auto *service = observer::ObServer::get_instance().get_gctx().schema_service_;
  bool row_exists = false, pending = false, failed_cast = false;
  Task failed_task;
  int64_t failed_max_retries = 0;
  ObSEArray<Task, 4> tasks;
  ObMySQLTransaction transaction;
  if (service == nullptr) { ret = OB_NOT_INIT;
  } else if (OB_FAIL(service->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(group.table_id_, table))) {
  } else if (table != nullptr && OB_FAIL(table_name(guard, *table, qualified, database))) {
  }
  if (OB_SUCC(ret)) { ret = transaction.start(sql_); }
  // Match the business DML lock order: business row, request group, tasks.
  if (OB_SUCC(ret) && table != nullptr) {
    ObSqlString query;
    if (OB_FAIL(query.append_fmt("SELECT 1 FROM %s WHERE %.*s FOR UPDATE",
            qualified.ptr(), group.predicate_.length(), group.predicate_.ptr()))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, result) {
        if (OB_FAIL(transaction.read(result, query.ptr()))) {
        } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result.get_result()->next())) {
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        } else { row_exists = true; }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObSqlString query;
    if (OB_FAIL(query.append_fmt("SELECT lease_epoch FROM oceanbase.__all_ai_column_group WHERE table_id=%lu AND request_id=", group.table_id_))) {
    } else if (OB_FAIL(hex_literal(query, group.request_))) {
    } else if (OB_FAIL(query.append_fmt(" AND lease_epoch=%ld AND lease_until>NOW(6) AND lease_owner=", group.epoch_))) {
    } else if (OB_FAIL(hex_literal(query, owner_))) {
    } else if (OB_FAIL(query.append(" FOR UPDATE"))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, result) {
        if (OB_FAIL(transaction.read(result, query.ptr()))) {
        } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result.get_result()->next())) {
          if (ret == OB_ITER_END) { ret = OB_EAGAIN; }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObSqlString query;
    if (OB_FAIL(query.append_fmt("SELECT column_id,definition_version,state,retry_count FROM oceanbase.__all_ai_column_task "
            "WHERE table_id=%lu AND request_id=", group.table_id_))) {
    } else if (OB_FAIL(hex_literal(query, group.request_))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, result) {
        ObMySQLResult *row = nullptr;
        if (OB_FAIL(transaction.read(result, query.ptr()))) {
        } else if ((row = result.get_result()) == nullptr) { ret = OB_ERR_UNEXPECTED;
        } else {
          while (OB_SUCC(ret) && OB_SUCC(ret = row->next())) {
            Task task;
            if (tasks.count() >= 4096) { ret = OB_SIZE_OVERFLOW;
            } else if (OB_FAIL(row->get_uint("column_id", task.column_id_))) {
            } else if (OB_FAIL(row->get_int("definition_version", task.definition_))) {
            } else if (OB_FAIL(row->get_int("state", task.state_))) {
            } else if (OB_FAIL(row->get_int("retry_count", task.retries_))) {
            } else if (OB_FAIL(tasks.push_back(task))) {
            } else if (task.state_ != 1 && task.state_ != 2) { pending = true; }
          }
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && !pending && row_exists && i < tasks.count(); ++i) {
    const Task &task = tasks.at(i);
    const AIColumnSchema *ai = table->get_ai_column(task.column_id_);
    const ObColumnSchemaV2 *result_column = table->get_column_schema(task.column_id_);
    if (ai == nullptr || result_column == nullptr || ai->definition_version_ != task.definition_) { continue; }
    ObArenaAllocator allocator;
    ObString output;
    int64_t final_state = task.state_;
    if (final_state == 1) {
      ObSqlString query;
      if (OB_FAIL(query.append_fmt("SELECT outcome FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND column_id=%lu AND request_id=", group.table_id_, task.column_id_))) {
      } else if (OB_FAIL(hex_literal(query, group.request_))) {
      } else {
        SMART_VAR(ObMySQLProxy::MySQLResult, result) {
          if (OB_FAIL(transaction.read(result, query.ptr()))) {
          } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(result.get_result()->next())) {
          } else { ret = read_text(*result.get_result(), "outcome", allocator, output, OUTPUT_LIMIT); }
        }
      }
      if (OB_SUCC(ret) && check_owner(guard, *table, *ai, database, allocator) != OB_SUCCESS) {
        final_state = 2;
      } else if (OB_SUCC(ret)) {
        ret = validate_output(allocator, *result_column, *ai, output);
        if (ret != OB_SUCCESS) { failed_cast = true; }
      }
    }
    if (OB_SUCC(ret)) {
      ObSqlString update;
      const ObColumnSchemaV2 *state = table->get_column_schema(ai->state_column_id_);
      const ObColumnSchemaV2 *request = table->get_column_schema(ai->request_column_id_);
      const ObColumnSchemaV2 *spec = table->get_column_schema(ai->result_spec_column_id_);
      if (state == nullptr || request == nullptr || spec == nullptr) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(update.append_fmt("UPDATE %s SET ", qualified.ptr()))) {
      } else if (OB_FAIL(identifier(update, result_column->get_column_name_str()))) {
      } else if (OB_FAIL(update.append("="))) {
      } else if (final_state == 1) {
        if (OB_FAIL(update.append("CONVERT("))) {
        } else if (OB_FAIL(hex_literal(update, output))) {
        } else { ret = update.append(" USING utf8mb4)"); }
      } else { ret = update.append("NULL"); }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(update.append(","))) {
      } else if (OB_FAIL(identifier(update, state->get_column_name_str()))) {
      } else if (OB_FAIL(update.append_fmt("=%ld WHERE %.*s AND ", final_state,
              group.predicate_.length(), group.predicate_.ptr()))) {
      } else if (OB_FAIL(identifier(update, request->get_column_name_str()))) {
      } else if (OB_FAIL(update.append("="))) {
      } else if (OB_FAIL(hex_literal(update, group.request_))) {
      } else if (OB_FAIL(update.append(" AND "))) {
      } else if (OB_FAIL(identifier(update, spec->get_column_name_str()))) {
      } else if (OB_FAIL(update.append_fmt("='%ld'", task.definition_))) {
      } else {
        auto *connection = sql::as_inner_sql_connection(transaction.get_connection());
        int64_t affected = 0;
        if (connection == nullptr) { ret = OB_ERR_UNEXPECTED;
        } else {
          ret = connection->execute_ai_column_write(update.string(), affected);
          if (ret != OB_SUCCESS) { failed_cast = true; }
        }
      }
    }
    if (failed_cast) { failed_task = task; failed_max_retries = ai->max_retries_; }
  }
  if (OB_SUCC(ret) && !pending) {
    ObSqlString erase;
    int64_t affected = 0;
    if (OB_FAIL(erase.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND request_id=", group.table_id_))) {
    } else if (OB_FAIL(hex_literal(erase, group.request_))) {
    } else if (OB_FAIL(transaction.write(erase.ptr(), affected))) {
    } else if (OB_FAIL(erase.assign_fmt("DELETE FROM oceanbase.__all_ai_column_group WHERE table_id=%lu AND request_id=", group.table_id_))) {
    } else if (OB_FAIL(hex_literal(erase, group.request_))) {
    } else if (OB_FAIL(erase.append_fmt(" AND lease_epoch=%ld AND lease_owner=", group.epoch_))) {
    } else if (OB_FAIL(hex_literal(erase, owner_))) {
    } else { ret = transaction.write(erase.ptr(), affected); }
  }
  if (transaction.is_started()) {
    const int end_ret = transaction.end(OB_SUCC(ret) && !pending);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  if (failed_cast && !cancellation_.load()) {
    const int save_ret = save_outcome(group, failed_task, ret, ObString(), failed_max_retries);
    if (save_ret != OB_SUCCESS) { ret = save_ret; }
  }
  if (OB_SUCC(ret) && pending) { ret = release_group(group); }
  return ret;
}

} } // namespace oceanbase::omt
