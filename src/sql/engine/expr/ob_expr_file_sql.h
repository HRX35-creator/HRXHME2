/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#pragma once
#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase { namespace sql {
class ExprFileSql : public ObFuncExprOperator {
public:
  explicit ExprFileSql(common::ObIAllocator &allocator);
  int calc_result_type1(ObExprResType &type, ObExprResType &argument, common::ObExprTypeCtx &ctx) const override;
  int cg_expr(ObExprCGCtx &ctx, const ObRawExpr &raw, ObExpr &expr) const override;
  static int eval_scalar(const ObExpr &expr, ObEvalCtx &ctx, common::ObDatum &result);
};
} }
