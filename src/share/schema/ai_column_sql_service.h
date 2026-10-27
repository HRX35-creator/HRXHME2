/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "common/mysqlclient/ob_isql_client.h"
namespace oceanbase { namespace share { namespace schema {
class ObTableSchema;
class AIColumnSQLService final
{
public:
  // The caller supplies the DDL transaction. Definitions, visible metadata and
  // schema-version snapshots are committed or rolled back together.
  static int store_metadata(common::ObISQLClient &client, const ObTableSchema &table);
  static int delete_current_metadata(common::ObISQLClient &client, uint64_t table_id);
};
} } }
