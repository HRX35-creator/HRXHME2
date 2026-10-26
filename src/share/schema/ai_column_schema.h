/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include "share/schema/ob_schema_struct.h"

namespace oceanbase { namespace share { namespace schema {

// Owned definition carried with a table schema. Runtime task state belongs to
// the transactional task tables, never to this immutable schema object.
class AIColumnSchema final : public ObSchema
{
  OB_UNIS_VERSION_V(1);
public:
  AIColumnSchema();
  explicit AIColumnSchema(common::ObIAllocator *allocator);
  ~AIColumnSchema() override = default;
  AIColumnSchema(const AIColumnSchema &) = delete;
  AIColumnSchema &operator=(const AIColumnSchema &) = delete;

  void reset() override;
  int assign(const AIColumnSchema &other);
  int64_t get_convert_size() const override;
  bool is_valid() const override;

  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  int64_t definition_version_;
  uint64_t owner_user_id_;
  int64_t max_retries_;
  bool embedding_;
  int64_t vector_dimension_;

  int set_column_name(const common::ObString &value) { return deep_copy_str(value, column_name_); }
  int set_model_name(const common::ObString &value) { return deep_copy_str(value, model_name_); }
  int set_original_expr(const common::ObString &value) { return deep_copy_str(value, original_expr_); }
  int set_normalized_expr(const common::ObString &value) { return deep_copy_str(value, normalized_expr_); }
  int set_input_expr(const common::ObString &value) { return deep_copy_str(value, input_expr_); }
  int set_system_prompt(const common::ObString &value) { return deep_copy_str(value, system_prompt_); }
  const common::ObString &get_system_prompt() const { return system_prompt_; }
  int set_config_expr(const common::ObString &value) { return deep_copy_str(value, config_expr_); }
  const common::ObString &get_column_name() const { return column_name_; }
  const common::ObString &get_model_name() const { return model_name_; }
  const common::ObString &get_original_expr() const { return original_expr_; }
  const common::ObString &get_normalized_expr() const { return normalized_expr_; }
  const common::ObString &get_input_expr() const { return input_expr_; }
  const common::ObString &get_config_expr() const { return config_expr_; }
  int set_dependencies(const common::ObIArray<uint64_t> &ids);
  int64_t get_dependency_count() const { return dependency_count_; }
  const uint64_t *get_dependencies() const { return dependencies_; }
  bool depends_on(uint64_t column_id) const;

  TO_STRING_KV(K_(column_id), K_(state_column_id), K_(request_column_id),
      K_(result_spec_column_id), K_(definition_version), K_(owner_user_id),
      K_(max_retries), K_(embedding), K_(vector_dimension), K_(column_name),
      K_(model_name), K_(dependency_count));
private:
  common::ObString column_name_;
  common::ObString model_name_;
  common::ObString original_expr_;
  common::ObString normalized_expr_;
  common::ObString input_expr_;
  common::ObString config_expr_;
  common::ObString system_prompt_;
  int64_t dependency_count_;
  uint64_t *dependencies_;
};

} } }
