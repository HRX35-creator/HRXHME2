/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_file_sql.h"
namespace oceanbase { namespace sql {
using namespace common;
ExprFileSql::ExprFileSql(ObIAllocator &allocator)
    : ObFuncExprOperator(allocator, T_FUN_SYS_FILE_SCAN, "__file_sql_scan", 1,
                         NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION) {}
int ExprFileSql::calc_result_type1(ObExprResType &type, ObExprResType &argument, ObExprTypeCtx &ctx) const
{
  UNUSED(ctx);
  argument.set_calc_type(ObVarcharType);
  argument.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  type.set_varchar();
  type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  type.set_collation_level(CS_LEVEL_IMPLICIT);
  return OB_SUCCESS;
}
int ExprFileSql::cg_expr(ObExprCGCtx &ctx, const ObRawExpr &raw, ObExpr &expr) const
{
  UNUSED(ctx);
  UNUSED(raw);
  expr.eval_func_ = eval_scalar;
  return OB_SUCCESS;
}
int ExprFileSql::eval_scalar(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &result)
{
  UNUSED(expr); UNUSED(ctx); UNUSED(result);
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "file readers must be used in FROM");
  return OB_NOT_SUPPORTED;
}
} }
