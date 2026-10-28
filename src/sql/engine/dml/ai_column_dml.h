/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{
class ObExecContext;
class ObEvalCtx;
struct ObDMLBaseCtDef;
struct AiColumnChange
{
  common::ObString invalidate_sql_;
  common::ObString cleanup_sql_;
  common::ObString enqueue_sql_;
};

class AiColumnDml
{
public:
  static int check_strict_read(ObExecContext &ctx);
  static int retry(ObExecContext &ctx, uint64_t table_id, uint64_t column_id,
                   const common::ObString &predicate);
  // Called before the statement savepoint is released. All writes use the
  // business session's transaction; no model request is made on this path.
  static int finish_statement(ObExecContext &ctx, bool rollback);
  static int record_row(const ObDMLBaseCtDef &definition, ObEvalCtx &eval_ctx,
                        ObExecContext &ctx, bool inserting, bool deleting);
};
} // namespace sql
} // namespace oceanbase
