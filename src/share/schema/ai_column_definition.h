/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/hash/ob_hashmap.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// This is schema data, never an executable generated-column default.  Strings
// decoded from the wire borrow the input buffer; ObColumnSchemaV2 owns that buffer.
struct AiColumnDefinition
{
  OB_UNIS_VERSION(1);
public:
  AiColumnDefinition() { reset(); }
  void reset()
  {
    expression_.reset();
    normalized_expression_.reset();
    type_prompt_.reset();
    dependency_ids_.reset();
    dependency_names_.reset();
    max_retries_ = 2;
    vector_dimension_ = 0;
    definition_version_ = 1;
    state_column_id_ = common::OB_INVALID_ID;
    request_column_id_ = common::OB_INVALID_ID;
    result_spec_column_id_ = common::OB_INVALID_ID;
  }
  int remap_column_ids(const common::hash::ObHashMap<uint64_t, uint64_t> &ids);
  bool has_dependency(uint64_t id) const
  {
    bool found = false;
    for (int64_t i = 0; !found && i < dependency_ids_.count(); ++i) {
      found = dependency_ids_.at(i) == id;
    }
    return found;
  }
  common::ObString expression_;
  common::ObString normalized_expression_;
  common::ObString type_prompt_;
  common::ObSEArray<uint64_t, 4> dependency_ids_;
  common::ObSEArray<common::ObString, 4> dependency_names_;
  int64_t max_retries_;
  int64_t vector_dimension_;
  int64_t definition_version_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
};

enum class AiColumnState : int64_t
{
  PENDING = 0,
  READY = 1,
  FAILED = 2
};

struct AiColumnReference
{
  OB_UNIS_VERSION(1);
public:
  AiColumnReference() : table_id_(common::OB_INVALID_ID), column_id_(common::OB_INVALID_ID) {}
  AiColumnReference(uint64_t table, uint64_t column) : table_id_(table), column_id_(column) {}
  uint64_t table_id_;
  uint64_t column_id_;
};

// Only kernel-issued maintenance SQL may assign AI results or hidden state.
class AiColumnExecutionGuard
{
public:
  explicit AiColumnExecutionGuard(const common::ObString *prompt = nullptr)
    : previous_prompt_(prompt_)
  {
    ++depth_;
    if (prompt != nullptr) { prompt_ = prompt; }
  }
  ~AiColumnExecutionGuard() { prompt_ = previous_prompt_; --depth_; }
  static bool active() { return depth_ > 0; }
  static common::ObString type_prompt() { return prompt_ == nullptr ? common::ObString() : *prompt_; }
private:
  static thread_local int64_t depth_;
  static thread_local const common::ObString *prompt_;
  const common::ObString *previous_prompt_;
};

// RETRY executes ordinary SQL predicates and row locking, but explicitly marks
// the one result column to invalidate even when business values do not change.
class AiColumnRetryGuard
{
public:
  explicit AiColumnRetryGuard(uint64_t table, uint64_t column)
    : previous_table_(table_), previous_column_(column_) { table_ = table; column_ = column; }
  ~AiColumnRetryGuard() { table_ = previous_table_; column_ = previous_column_; }
  static uint64_t column(uint64_t table) { return table_ == table ? column_ : common::OB_INVALID_ID; }
  static bool active() { return table_ != common::OB_INVALID_ID; }
private:
  static thread_local uint64_t table_;
  static thread_local uint64_t column_;
  uint64_t previous_table_;
  uint64_t previous_column_;
};

} // namespace schema
} // namespace share
} // namespace oceanbase
