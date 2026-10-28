/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/table_function/table_function.h"

namespace oceanbase { namespace sql {
class ObDMLResolver;
int bind_file_table_function(ObDMLResolver &, const TableFunctionCall &, TableItem *&);
int create_file_table_scan(const TableFunctionScanContext &, ITableFunctionScan *&);
}}
