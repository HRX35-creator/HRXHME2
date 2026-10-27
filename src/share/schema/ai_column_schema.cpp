/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include "ai_column_schema.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase { namespace share { namespace schema {
using namespace common;

AIColumnSchema::AIColumnSchema() : ObSchema() { reset(); }
AIColumnSchema::AIColumnSchema(ObIAllocator *allocator) : ObSchema(allocator) { reset(); }

void AIColumnSchema::reset()
{
  column_id_ = state_column_id_ = request_column_id_ = result_spec_column_id_ = OB_INVALID_ID;
  definition_version_ = 0; owner_user_id_ = OB_INVALID_ID;
  max_retries_ = 2; embedding_ = false; vector_dimension_ = 0;
  column_name_.reset(); model_name_.reset(); original_expr_.reset(); normalized_expr_.reset();
  input_expr_.reset(); config_expr_.reset(); system_prompt_.reset(); dependency_count_ = 0; dependencies_ = nullptr;
  ObSchema::reset();
}

bool AIColumnSchema::is_valid() const
{
  return error_ret_ == OB_SUCCESS && column_id_ != OB_INVALID_ID
      && state_column_id_ != OB_INVALID_ID && request_column_id_ != OB_INVALID_ID
      && result_spec_column_id_ != OB_INVALID_ID && owner_user_id_ != OB_INVALID_ID
      && column_id_ != state_column_id_ && column_id_ != request_column_id_
      && column_id_ != result_spec_column_id_ && state_column_id_ != request_column_id_
      && state_column_id_ != result_spec_column_id_ && request_column_id_ != result_spec_column_id_
      && !column_name_.empty() && !model_name_.empty() && !original_expr_.empty()
      && !normalized_expr_.empty() && !input_expr_.empty() && max_retries_ >= 0
      && (!embedding_ || vector_dimension_ > 0)
      && dependency_count_ >= 0 && (dependency_count_ == 0 || dependencies_ != nullptr);
}

int AIColumnSchema::set_dependencies(const ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  uint64_t *copy = nullptr;
  if (ids.count() < 0 || ids.count() > 4096) { ret = OB_INVALID_ARGUMENT; }
  else if (ids.count() > 0 && (copy = static_cast<uint64_t *>(alloc(ids.count() * sizeof(uint64_t)))) == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    if (ids.at(i) == OB_INVALID_ID) { ret = OB_INVALID_ARGUMENT; }
    else { copy[i] = ids.at(i); }
  }
  if (OB_SUCC(ret)) { dependencies_ = copy; dependency_count_ = ids.count(); }
  return ret;
}

bool AIColumnSchema::depends_on(uint64_t column_id) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < dependency_count_; ++i) { found = dependencies_[i] == column_id; }
  return found;
}

int AIColumnSchema::assign(const AIColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    reset();
    column_id_ = other.column_id_; state_column_id_ = other.state_column_id_;
    request_column_id_ = other.request_column_id_; result_spec_column_id_ = other.result_spec_column_id_;
    definition_version_ = other.definition_version_; owner_user_id_ = other.owner_user_id_;
    max_retries_ = other.max_retries_; embedding_ = other.embedding_; vector_dimension_ = other.vector_dimension_;
    ObSEArray<uint64_t, 4> dependencies;
    if (other.dependency_count_ < 0 || other.dependency_count_ > 4096
        || (other.dependency_count_ > 0 && other.dependencies_ == nullptr)) { ret = OB_INVALID_ARGUMENT; }
    for (int64_t i = 0; OB_SUCC(ret) && i < other.dependency_count_; ++i) {
      ret = dependencies.push_back(other.dependencies_[i]);
    }
    if (other.error_ret_ != OB_SUCCESS) { ret = other.error_ret_; }
    else if (OB_FAIL(ret)) {
    }
    else if (OB_FAIL(set_column_name(other.column_name_))) {
    } else if (OB_FAIL(set_model_name(other.model_name_))) {
    } else if (OB_FAIL(set_original_expr(other.original_expr_))) {
    } else if (OB_FAIL(set_normalized_expr(other.normalized_expr_))) {
    } else if (OB_FAIL(set_input_expr(other.input_expr_))) {
    } else if (OB_FAIL(set_config_expr(other.config_expr_))) {
    } else if (OB_FAIL(set_system_prompt(other.system_prompt_))) {
    } else if (OB_FAIL(set_dependencies(dependencies))) {
    }
    error_ret_ = ret;
  }
  return ret;
}

int64_t AIColumnSchema::get_convert_size() const
{
  return sizeof(*this) + column_name_.length() + model_name_.length() + original_expr_.length()
      + normalized_expr_.length() + input_expr_.length() + config_expr_.length() + system_prompt_.length() + 7
      + dependency_count_ * sizeof(uint64_t);
}

OB_DEF_SERIALIZE(AIColumnSchema)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, column_id_, state_column_id_, request_column_id_, result_spec_column_id_,
      definition_version_, owner_user_id_, max_retries_, embedding_, vector_dimension_, column_name_,
      model_name_, original_expr_, normalized_expr_, input_expr_, config_expr_, system_prompt_, dependency_count_);
  for (int64_t i = 0; OB_SUCC(ret) && i < dependency_count_; ++i) { OB_UNIS_ENCODE(dependencies_[i]); }
  return ret;
}

OB_DEF_DESERIALIZE(AIColumnSchema)
{
  int ret = OB_SUCCESS;
  reset();
  ObString name, model, original, normalized, input, config, system_prompt;
  int64_t count = 0;
  LST_DO_CODE(OB_UNIS_DECODE, column_id_, state_column_id_, request_column_id_, result_spec_column_id_,
      definition_version_, owner_user_id_, max_retries_, embedding_, vector_dimension_, name,
      model, original, normalized, input, config, system_prompt, count);
  ObSEArray<uint64_t, 4> ids;
  if (OB_SUCC(ret) && (count < 0 || count > 4096)) { ret = OB_DESERIALIZE_ERROR; }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    uint64_t id = OB_INVALID_ID;
    OB_UNIS_DECODE(id);
    if (OB_SUCC(ret)) { ret = ids.push_back(id); }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(set_column_name(name))) {
  } else if (OB_FAIL(set_model_name(model))) {
  } else if (OB_FAIL(set_original_expr(original))) {
  } else if (OB_FAIL(set_normalized_expr(normalized))) {
  } else if (OB_FAIL(set_input_expr(input))) {
  } else if (OB_FAIL(set_config_expr(config))) {
  } else if (OB_FAIL(set_system_prompt(system_prompt))) {
  } else if (OB_FAIL(set_dependencies(ids))) {
  }
  error_ret_ = ret;
  return ret;
}

OB_DEF_SERIALIZE_SIZE(AIColumnSchema)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, column_id_, state_column_id_, request_column_id_, result_spec_column_id_,
      definition_version_, owner_user_id_, max_retries_, embedding_, vector_dimension_, column_name_,
      model_name_, original_expr_, normalized_expr_, input_expr_, config_expr_, system_prompt_, dependency_count_);
  for (int64_t i = 0; i < dependency_count_; ++i) { OB_UNIS_ADD_LEN(dependencies_[i]); }
  return len;
}

} } }
