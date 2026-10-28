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
#include "ob_expr_ai_prompt.h"
#include <regex>
#include "ob_ai_func.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"


using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

/**
 * @brief Check whether the type can be used as the template or as an argument
 *        of ai_prompt. The text and the longtext types are accepted, their
 *        binary variants are not.
 */
static bool is_valid_prompt_string_type(const ObExprResType &type)
{
  bool is_valid = false;
  const ObObjType obj_type = type.get_type();
  if (ob_is_string_tc(obj_type)) {
    // varchar, char and the other string types
    is_valid = true;
  } else if (ob_is_text_tc(obj_type)) {
    // text, mediumtext and longtext are accepted, the blob variants are not
    is_valid = (CS_TYPE_BINARY != type.get_collation_type());
  }
  return is_valid;
}

/**
 * @brief Append a json argument to the argument list of a prompt object.
 *        A nested prompt object is rendered into a plain string argument, any
 *        other json value is kept as json.
 */
static int append_json_prompt_arg(const ObExpr &expr,
                                  ObEvalCtx &ctx,
                                  MultimodeAlloctor &allocator,
                                  int64_t arg_idx,
                                  ObJsonArray *args_array)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = nullptr;
  bool is_null = false;
  if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator,
              static_cast<uint16_t>(arg_idx), j_base, is_null))) {
  } else if (is_null || OB_ISNULL(j_base)) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
  } else {
    ObJsonNode *arg_node = static_cast<ObJsonNode *>(j_base);
    if (ObJsonNodeType::J_OBJECT == arg_node->json_type()
        && ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(arg_node))) {
      // a nested prompt object is rendered with its own arguments first
      ObString expanded_str;
      if (OB_FAIL(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                  allocator, static_cast<ObJsonObject *>(arg_node), expanded_str))) {
      } else {
        ObJsonString *arg_json_str = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, expanded_str, arg_json_str))) {
        } else if (OB_FAIL(args_array->append(arg_json_str))) {
        }
      }
    } else if (OB_FAIL(args_array->append(arg_node))) {
    }
  }
  return ret;
}

ObExprAIPrompt::ObExprAIPrompt(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_PROMPT, 
                    N_AI_PROMPT, 
                    MORE_THAN_ZERO,
                    VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIPrompt::~ObExprAIPrompt() 
{
}

int ObExprAIPrompt::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const 
{
  int ret = OB_SUCCESS;
  ObExprResType &template_type = types_stack[0];
  if (!is_valid_prompt_string_type(template_type)) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
  } else {
    if (ob_is_string_tc(template_type.get_type())) {
      // the char type is evaluated as a varchar, the text type keeps its own
      // type because its content is read from the lob
      template_type.set_calc_type(ObVarcharType);
    }
    template_type.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  }

  for (int64_t i = 1; i < param_num && OB_SUCC(ret); i++) {
    ObExprResType &arg_type = types_stack[i];
    if (is_valid_prompt_string_type(arg_type)) {
      if (ob_is_string_tc(arg_type.get_type())) {
        arg_type.set_calc_type(ObVarcharType);
      }
      arg_type.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (ob_is_json(arg_type.get_type())) {
      // a json argument is kept as json, it may hold a nested prompt object
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
    }
  }

  if (OB_SUCC(ret)) {
    type.set_json();
    type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
  }
  return ret;
}

int ObExprAIPrompt::eval_ai_prompt(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *template_datum = NULL;
  ObExpr *arg0 = expr.args_[0];
  ObString template_str;
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor tmp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_PROMPT));

  if (OB_FAIL(tmp_allocator.eval_arg(arg0, ctx, template_datum))) {
  } else if (template_datum->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt, template is null");
    res.set_null();
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
             ctx.exec_ctx_, tmp_allocator, *template_datum, arg0->datum_meta_,
             arg0->obj_meta_.has_lob_header(), template_str))) {
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(tmp_allocator, args_array))) {
  } else {
    for (int64_t i = 1; i < expr.arg_cnt_ && OB_SUCC(ret); i++) {
      ObDatum *datum = NULL;
      ObExpr *arg = expr.args_[i];
      ObString arg_str;
      ObJsonString *arg_json_str = NULL;
      if (OB_FAIL(tmp_allocator.eval_arg(arg, ctx, datum))) {
      } else if (datum->is_null()) {
        ret = OB_ERR_INVALID_TYPE_FOR_OP;
      } else if (ob_is_string_tc(arg->datum_meta_.type_) || ob_is_text_tc(arg->datum_meta_.type_)) {
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                ctx.exec_ctx_, tmp_allocator, *datum, arg->datum_meta_,
                arg->obj_meta_.has_lob_header(), arg_str))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(tmp_allocator, arg_str, arg_json_str))) {
        } else if (OB_FAIL(args_array->append(arg_json_str))) {
        }
      } else if (ob_is_json(arg->datum_meta_.type_)) {
        if (OB_FAIL(append_json_prompt_arg(expr, ctx, tmp_allocator, i, args_array))) {
        }
      } else {
        ret = OB_ERR_INVALID_TYPE_FOR_OP;
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(tmp_allocator, template_str, args_array, prompt_object))) {
  } else {
    ObString raw_str;
    if (OB_FAIL(prompt_object->get_raw_binary(raw_str, &tmp_allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}



int ObExprAIPrompt::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
    UNUSED(expr_cg_ctx);
    UNUSED(raw_expr);
    int ret = OB_SUCCESS;
    rt_expr.eval_func_ = ObExprAIPrompt::eval_ai_prompt;
    return ret;
}

} // namespace sql
} // namespace oceanbase
