#pragma once

#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase
{
namespace sql
{

class ObExprFileTableBase : public ObFuncExprOperator
{
public:
  ObExprFileTableBase(common::ObIAllocator &alloc,
                      ObExprOperatorType type,
                      const char *name)
      : ObFuncExprOperator(alloc, type, name, MORE_THAN_ZERO,
                           NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
  {
  }

  virtual int calc_result_typeN(ObExprResType &type,
                                ObExprResType *types_stack,
                                int64_t param_num,
                                common::ObExprTypeCtx &type_ctx) const override
  {
    int ret = common::OB_SUCCESS;
    UNUSED(types_stack);
    UNUSED(param_num);
    UNUSED(type_ctx);
    type.set_varchar();
    type.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(common::CS_LEVEL_COERCIBLE);
    return ret;
  }

  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override
  {
    int ret = common::OB_SUCCESS;
    UNUSED(expr_cg_ctx);
    UNUSED(raw_expr);
    rt_expr.eval_func_ = eval_file_table;
    return ret;
  }

  static int eval_file_table(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
  {
    int ret = common::OB_SUCCESS;
    UNUSED(expr);
    UNUSED(ctx);
    res.set_null();
    return ret;
  }
};

class ObExprReadCsv : public ObExprFileTableBase
{
public:
  explicit ObExprReadCsv(common::ObIAllocator &alloc)
      : ObExprFileTableBase(alloc, T_FUN_SYS_READ_CSV, "read_csv")
  {
  }
};

class ObExprReadJson : public ObExprFileTableBase
{
public:
  explicit ObExprReadJson(common::ObIAllocator &alloc)
      : ObExprFileTableBase(alloc, T_FUN_SYS_READ_JSON, "read_json")
  {
  }
};

class ObExprReadParquet : public ObExprFileTableBase
{
public:
  explicit ObExprReadParquet(common::ObIAllocator &alloc)
      : ObExprFileTableBase(alloc, T_FUN_SYS_READ_PARQUET, "read_parquet")
  {
  }
};

class ObExprGlob : public ObExprFileTableBase
{
public:
  explicit ObExprGlob(common::ObIAllocator &alloc)
      : ObExprFileTableBase(alloc, T_FUN_SYS_GLOB, "glob")
  {
  }
};

} // namespace sql
} // namespace oceanbase
