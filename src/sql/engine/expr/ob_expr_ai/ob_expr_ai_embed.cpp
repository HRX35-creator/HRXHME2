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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ob_is_unsigned_type(types_stack[DIM_IDX].get_type())
                                         ? ObUInt64Type : ObIntType);
        types_stack[DIM_IDX].set_precision(20);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::get_dimension(const ObExpr &expr, const ObDatum *datum, int64_t &dimension)
{
  int ret = OB_SUCCESS;
  dimension = 0;
  if (OB_ISNULL(datum)) {
    // No explicit dimension: let the provider choose its default.
  } else if (datum->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ob_is_unsigned_type(expr.args_[DIM_IDX]->datum_meta_.type_)) {
    uint64_t unsigned_dim = datum->get_uint();
    if (unsigned_dim == 0 || unsigned_dim > INT64_MAX) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      dimension = static_cast<int64_t>(unsigned_dim);
    }
  } else if ((dimension = datum->get_int()) <= 0) {
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_FAIL(ret)) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  int64_t dim = 0;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (OB_FAIL(get_dimension(expr, arg_dim, dim))) {
  }
  if (OB_FAIL(ret)) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                     const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatumVector results = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObArray<int64_t> pending_rows;
  ObArray<int64_t> dimensions;
  ObEvalCtx::BatchInfoScopeGuard batch_info(ctx);
  batch_info.set_batch_size(batch_size);
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, batch_size))) {
    }
  }
  if (OB_SUCC(ret)) {
    ObDatumVector models = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector contents = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector dims = expr.arg_cnt_ == 3
                        ? expr.args_[DIM_IDX]->locate_expr_datumvector(ctx) : ObDatumVector();
    // Validate every active row before sending any request from this batch.
    for (int64_t row = 0; OB_SUCC(ret) && row < batch_size; ++row) {
      if (skip.at(row) || eval_flags.at(row)) {
        continue;
      }
      int64_t dim = 0;
      if (OB_FAIL(get_dimension(expr, expr.arg_cnt_ == 3 ? dims.at(row) : nullptr, dim))) {
      } else if (models.at(row)->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
      } else if (contents.at(row)->is_null()) {
        results.at(row)->set_null();
        eval_flags.set(row);
      } else if (models.at(row)->get_string().empty() || contents.at(row)->get_string().empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      } else if (OB_FAIL(pending_rows.push_back(row))) {
      } else if (OB_FAIL(dimensions.push_back(dim))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < pending_rows.count(); ++i) {
      int64_t row = pending_rows.at(i);
      if (eval_flags.at(row)) {
        continue;
      }
      ObEvalCtx::TempAllocGuard alloc_guard(ctx);
      MultimodeAlloctor allocator(alloc_guard.get_allocator());
      lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
      ObString model_id = models.at(row)->get_string();
      int64_t dim = dimensions.at(i);
      ObArray<int64_t> group_rows;
      ObArray<ObString> group_contents;
      ObArray<ObString> group_results;
      for (int64_t j = i; OB_SUCC(ret) && j < pending_rows.count(); ++j) {
        int64_t candidate = pending_rows.at(j);
        if (!eval_flags.at(candidate) && dimensions.at(j) == dim
            && models.at(candidate)->get_string() == model_id) {
          if (OB_FAIL(group_rows.push_back(candidate))) {
          } else if (OB_FAIL(group_contents.push_back(contents.at(candidate)->get_string()))) {
          }
        }
      }
      ObJsonObject *config = nullptr;
      ObJsonInt *dim_json = nullptr;
      if (OB_SUCC(ret) && expr.arg_cnt_ == 3) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo endpoint;
      query::ObIAiEndpointResolver *resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
      } else if (OB_ISNULL(resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
      } else {
        ObAIFuncModel model(allocator, *info, endpoint);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(group_contents, config, group_results))) {
        } else if (group_results.count() != group_rows.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t j = 0; OB_SUCC(ret) && j < group_rows.count(); ++j) {
            int64_t result_row = group_rows.at(j);
            batch_info.set_batch_idx(result_row);
            if (OB_FAIL(ObAIFuncUtils::set_string_result(
                    expr, ctx, *results.at(result_row), group_results.at(j)))) {
            } else {
              eval_flags.set(result_row);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
