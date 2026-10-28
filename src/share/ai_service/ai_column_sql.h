/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace share
{
// Identifiers and values are encoded independently of sql_mode. These helpers
// never treat model output, a primary key, or a user name as executable SQL.
inline int ai_column_identifier(common::ObSqlString &sql, const common::ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') {
      ret = sql.append("``");
    } else {
      ret = sql.append(name.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

inline int ai_column_literal(common::ObSqlString &sql, const common::ObString &value)
{
  int ret = sql.append("X'");
  const char digits[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char byte = static_cast<unsigned char>(value.ptr()[i]);
    const char hex[] = {digits[byte >> 4], digits[byte & 15]};
    ret = sql.append(hex, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}
} // namespace share
} // namespace oceanbase
