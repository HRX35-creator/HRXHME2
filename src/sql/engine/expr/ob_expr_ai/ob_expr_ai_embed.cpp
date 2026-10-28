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
#include "lib/utility/utility.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

/**
 * @brief Check whether the two model ids are the same.
 */
static bool is_same_model_id(const ObString &left, const ObString &right)
{
  return left.length() == right.length()
         && (left.length() == 0 || 0 == MEMCMP(left.ptr(), right.ptr(), left.length()));
}

/**
 * @brief Read the real content of the argument, the content of a text or a
 *        longtext argument is read from the lob.
 */
static int read_real_content(ObEvalCtx &ctx,
                             ObIAllocator &allocator,
                             const ObExpr *arg_expr,
                             const ObDatum &datum,
                             ObString &content)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(arg_expr)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ob_is_text_tc(arg_expr->datum_meta_.type_)) {
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, datum,
                  arg_expr->datum_meta_, arg_expr->obj_meta_.has_lob_header(), content))) {
    }
  } else {
    content = datum.get_string();
  }
  return ret;
}

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
    ObObjType content_type = types_stack[CONTENT_IDX].get_type();
    if (ob_is_text_tc(content_type)
        && CS_TYPE_BINARY != types_stack[CONTENT_IDX].get_collation_type()) {
      // the text and the longtext keep their own type, their content is read
      // from the lob when the function is evaluated
    } else {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
      types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
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

int ObExprAIEmbed::call_model_and_set_result(const ObExpr &expr,
                                             ObEvalCtx &ctx,
                                             common::ObIAllocator &allocator,
                                             const common::ObString &model_id,
                                             const int64_t dimension,
                                             common::ObArray<common::ObString> &contents,
                                             common::ObArray<common::ObString> &results)
{
  int ret = OB_SUCCESS;
  ObJsonObject *config = nullptr;
  ObJsonInt *dim_json = nullptr;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  UNUSED(expr);
  UNUSED(ctx);
  if (dimension > 0) {
    // the dimension is sent to the model, the dimension of the answer is
    // checked when the answer is parsed
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim_json))) {
    } else if (OB_FAIL(config->add("dimensions", dim_json))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                 model_id, allocator, resolved_endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, *endpoint_info);
    if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
    } else if (results.count() != contents.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "ai_embed, the number of the answers is not equal to the number of the inputs");
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // the null of the input is propagated to the result
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString model_id = arg_model_id->get_string();
    ObString content;
    int64_t dim = 0;
    if (OB_FAIL(read_real_content(ctx, temp_allocator, expr.args_[CONTENT_IDX], *arg_content, content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      }
    }
    if (OB_SUCC(ret)) {
      ObArray<ObString> contents;
      ObArray<ObString> results;
      if (OB_FAIL(contents.push_back(content))) {
      } else if (OB_FAIL(call_model_and_set_result(expr, ctx, temp_allocator, model_id, dim, contents, results))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results[0]))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr,
                                        ObEvalCtx &ctx,
                                        const ObBitVector &skip,
                                        const int64_t size)
{
  int ret = OB_SUCCESS;
  ObDatumVector res_datum = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  // the rows that are sent to the model, the model id and the dimension of
  // the rows are collected to group the rows into the requests
  ObArray<int64_t> row_indexes;
  ObArray<ObString> row_models;
  ObArray<ObString> row_contents;
  ObArray<int64_t> row_dims;

  if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, size))
      || OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, size))
      || OB_FAIL(expr.arg_cnt_ > DIM_IDX && expr.args_[DIM_IDX]->eval_batch(ctx, skip, size))) {
  } else {
    ObDatumVector model_array = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector content_array = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector dim_array = expr.arg_cnt_ > DIM_IDX
                              ? expr.args_[DIM_IDX]->locate_expr_datumvector(ctx)
                              : ObDatumVector();
    for (int64_t j = 0; OB_SUCC(ret) && j < size; j++) {
      if (skip.at(j) || eval_flags.at(j)) {
        // the row is filtered out or it is evaluated already
        continue;
      }
      if (model_array.at(j)->is_null() || content_array.at(j)->is_null()) {
        // the null of the input is propagated to the result
        res_datum.at(j)->set_null();
        eval_flags.set(j);
        continue;
      }
      ObString model_id = model_array.at(j)->get_string();
      ObString content;
      int64_t dim = 0;
      if (OB_FAIL(read_real_content(ctx, temp_allocator, expr.args_[CONTENT_IDX],
                                    *content_array.at(j), content))) {
        break;
      }
      if (model_id.empty() || content.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        break;
      }
      if (expr.arg_cnt_ > DIM_IDX && !dim_array.at(j)->is_null()) {
        dim = dim_array.at(j)->get_int();
        if (dim <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          break;
        }
      }
      if (OB_FAIL(row_indexes.push_back(j))
          || OB_FAIL(row_models.push_back(model_id))
          || OB_FAIL(row_contents.push_back(content))
          || OB_FAIL(row_dims.push_back(dim))) {
        break;
      }
    }
  }

  if (OB_SUCC(ret)) {
    // the rows that use the same model and the same dimension are sent to the
    // model in one request, the request is split when it carries too many
    // inputs
    const int64_t row_count = row_indexes.count();
    // the rows are grouped by the model id and the dimension, the rows of one
    // group are not required to be adjacent to each other
    ObArray<int64_t> row_group_ids;
    ObArray<ObString> group_models;
    ObArray<int64_t> group_dims;
    for (int64_t i = 0; OB_SUCC(ret) && i < row_count; i++) {
      int64_t group_id = -1;
      for (int64_t g = 0; g < group_models.count(); g++) {
        if (group_dims[g] == row_dims[i]
            && is_same_model_id(group_models[g], row_models[i])) {
          group_id = g;
          break;
        }
      }
      if (group_id < 0) {
        if (OB_FAIL(group_models.push_back(row_models[i]))
            || OB_FAIL(group_dims.push_back(row_dims[i]))) {
          break;
        }
        group_id = group_models.count() - 1;
      }
      if (OB_FAIL(row_group_ids.push_back(group_id))) {
        break;
      }
    }
    for (int64_t g = 0; OB_SUCC(ret) && g < group_models.count(); g++) {
      // the rows of one group keep the order of the table scan
      ObArray<int64_t> group_row_pos;
      for (int64_t i = 0; OB_SUCC(ret) && i < row_count; i++) {
        if (row_group_ids[i] == g && OB_FAIL(group_row_pos.push_back(i))) {
          break;
        }
      }
      for (int64_t chunk_start = 0;
           OB_SUCC(ret) && chunk_start < group_row_pos.count();
           chunk_start += MAX_BATCH_CONTENT_CNT) {
        int64_t chunk_end = chunk_start + MAX_BATCH_CONTENT_CNT;
        if (chunk_end > group_row_pos.count()) {
          chunk_end = group_row_pos.count();
        }
        ObArray<ObString> contents;
        ObArray<ObString> results;
        for (int64_t i = chunk_start; OB_SUCC(ret) && i < chunk_end; i++) {
          if (OB_FAIL(contents.push_back(row_contents[group_row_pos[i]]))) {
            break;
          }
        }
        if (OB_SUCC(ret)
            && OB_FAIL(call_model_and_set_result(expr, ctx, temp_allocator,
                                                 group_models[g], group_dims[g],
                                                 contents, results))) {
          break;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < results.count(); i++) {
          const int64_t row_index = row_indexes[group_row_pos[chunk_start + i]];
          ObTextStringDatumResult str_result(expr.datum_meta_.type_, &expr, &ctx, res_datum.at(row_index));
          if (OB_FAIL(str_result.init_with_batch_idx(results[i].length(), row_index))) {
            break;
          } else if (OB_FAIL(str_result.append(results[i].ptr(), results[i].length()))) {
            break;
          } else {
            str_result.set_result();
            // the row is evaluated, the flag is set after the result is
            // written so that a row is never marked as evaluated without a
            // result
            eval_flags.set(row_index);
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
    // the rows of a table scan are sent to the model in one request
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
