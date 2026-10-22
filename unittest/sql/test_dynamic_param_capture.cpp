/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "query/engine/expr/ob_expr_lob_utils.h"
#include "query/engine/ob_operator.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace {
struct Frame {
  ObArenaAllocator arena;
  ObExecContext exec;
  ObExpr expr;
  char **frames;
  Frame() : arena(), exec(arena), expr(), frames(nullptr) {
    EXPECT_EQ(OB_SUCCESS, exec.create_physical_plan_ctx());
    EXPECT_EQ(OB_SUCCESS, exec.get_physical_plan_ctx()->reserve_param_space(1));
    EXPECT_EQ(OB_SUCCESS, exec.get_physical_plan_ctx()->init_datum_param_store());
    frames = static_cast<char **>(arena.alloc(sizeof(char *)));
    frames[0] = static_cast<char *>(arena.alloc(2048));
    memset(frames[0], 0, 2048);
    exec.set_frames(frames);
    expr.frame_idx_ = 0;
    expr.datum_off_ = 64;
    expr.eval_info_off_ = 128;
    expr.res_buf_off_ = 256;
    expr.res_buf_len_ = 512;
    expr.type_ = T_EXEC_VAR;
    expr.obj_meta_.set_int();
    expr.datum_meta_.type_ = ObIntType;
    expr.obj_datum_map_ = OBJ_DATUM_8BYTE_DATA;
    new (frames[0] + 64) ObDatum();
  }
  ObObjParam &legacy() { return exec.get_physical_plan_ctx()->get_param_store_for_update().at(0); }
  DatumParameter &native() { return exec.get_physical_plan_ctx()->get_datum_param_store().at(0); }
  void seed_native() { EXPECT_EQ(OB_SUCCESS, copy_legacy_parameter(legacy(), native(), arena)); }
  DatumParameter integer(int64_t n) {
    DatumParameter p;
    p.set_type_meta(expr.obj_meta_);
    EXPECT_EQ(OB_SUCCESS, p.alloc_datum_reserved_buff(expr.obj_meta_, -1, arena));
    p.datum_.set_int(n);
    return p;
  }
};
} // namespace
TEST(DynamicParamCapture, value_is_captured_from_sql_frame_not_old_value_payload) {
  Frame f;
  ObEvalCtx ctx(f.exec);
  ObDynamicParamSetter setter(0, nullptr, &f.expr);
  ObDatum &frame = f.expr.locate_datum_for_write(ctx);
  frame.set_int(42);
  f.legacy().set_int(999);
  f.legacy().set_precision(19);
  f.legacy().set_result_flag(0x1234);
  f.legacy().set_raw_text_info(3, 7);
  f.legacy().set_param_meta();
  f.seed_native();
  f.legacy().set_precision(9);
  DatumParameter view;
  ASSERT_EQ(OB_SUCCESS, setter.capture_dynamic_param(ctx, view));
  EXPECT_EQ(42, view.datum_.get_int());
  EXPECT_EQ(999, f.legacy().get_int());
  EXPECT_EQ(frame.ptr_, view.datum_.ptr_);
  EXPECT_EQ(19, view.get_precision());
  EXPECT_EQ(0x1234, view.get_result_flag());
  EXPECT_EQ(3, view.get_raw_text_pos());
  EXPECT_EQ(7, view.get_raw_text_len());
  EXPECT_EQ(f.legacy().get_param_meta(), view.get_param_meta());
  ObBatchRescanParams batch;
  ObBatchRescanParams::Row values(4 * sizeof(DatumParameter));
  ObSEArray<int64_t, 1> ids;
  ASSERT_EQ(OB_SUCCESS, ids.push_back(0));
  ASSERT_EQ(OB_SUCCESS, values.push_back(view));
  ASSERT_EQ(OB_SUCCESS, batch.append_batch_rescan_param(ids, values));
  frame.set_int(88);
  EXPECT_EQ(42, batch.get_one_batch_params(0).at(0).datum_.get_int());
}
TEST(DynamicParamCapture, restoring_snapshot_does_not_evaluate_source_again) {
  Frame f;
  ObEvalCtx ctx(f.exec);
  ObDynamicParamSetter setter(0, nullptr, &f.expr);
  DatumParameter source = f.integer(INT64_MIN);
  source.set_precision(19);
  source.set_raw_text_info(17, 23);
  source.set_result_flags(0xa5);
  ASSERT_EQ(OB_SUCCESS, setter.restore_dynamic_param(ctx, source));
  EXPECT_EQ(INT64_MIN, f.expr.locate_expr_datum(ctx).get_int());
  EXPECT_EQ(INT64_MIN, f.legacy().get_int());
  EXPECT_EQ(19, f.legacy().get_precision());
  EXPECT_EQ(0xa5, f.legacy().get_result_flag());
  EXPECT_EQ(17, f.legacy().get_raw_text_pos());
  EXPECT_EQ(23, f.legacy().get_raw_text_len());
  EXPECT_EQ(f.expr.get_rev_buf(ctx), f.expr.locate_expr_datum(ctx).ptr_);
}
TEST(DynamicParamCapture, typed_null_snapshot_clears_stale_param_meta_before_restore) {
  Frame f;
  ObEvalCtx ctx(f.exec);
  ObDynamicParamSetter setter(0, nullptr, &f.expr);
  f.legacy().set_int(99);
  f.legacy().set_param_meta();
  DatumParameter source;
  ASSERT_EQ(OB_SUCCESS, setter.restore_dynamic_param(ctx, source));
  EXPECT_TRUE(f.expr.locate_expr_datum(ctx).is_null());
  EXPECT_TRUE(f.legacy().is_null());
  EXPECT_TRUE(f.legacy().get_param_meta().is_null());
  ObObjMeta string_type;
  string_type.set_varchar();
  source.set_param_meta(string_type);
  source.set_length(65);
  ASSERT_EQ(OB_SUCCESS, setter.restore_dynamic_param(ctx, source));
  EXPECT_EQ(string_type, f.legacy().get_param_meta());
  EXPECT_EQ(65, f.legacy().get_length());
}
TEST(DynamicParamCapture, invalid_snapshot_locations_preserve_output) {
  Frame f;
  ObEvalCtx ctx(f.exec);
  DatumParameter output = f.integer(7);
  for (int64_t idx : {int64_t(-1), int64_t(1)}) {
    ObDynamicParamSetter bad(idx, nullptr, &f.expr);
    EXPECT_EQ(OB_INVALID_ARGUMENT, bad.capture_dynamic_param(ctx, output));
    EXPECT_EQ(7, output.datum_.get_int());
    EXPECT_EQ(OB_INVALID_ARGUMENT, bad.restore_dynamic_param(ctx, output));
  }
  ObDynamicParamSetter missing(0, nullptr, nullptr);
  EXPECT_EQ(OB_INVALID_ARGUMENT, missing.capture_dynamic_param(ctx, output));
  EXPECT_EQ(7, output.datum_.get_int());
}
TEST(DynamicParamCapture, px_worker_fills_expression_from_native_batch) {
  Frame f;
  ObBatchRescanParams batch;
  ObBatchRescanParams::Row row(4 * sizeof(DatumParameter));
  ObSEArray<int64_t, 1> ids, exprs;
  ASSERT_EQ(OB_SUCCESS, ids.push_back(0));
  ASSERT_EQ(OB_SUCCESS, exprs.push_back(1));
  ASSERT_EQ(OB_SUCCESS, row.push_back(f.integer(123)));
  ASSERT_EQ(OB_SUCCESS, batch.append_batch_rescan_param(ids, row, exprs));
  ObSEArray<ObExpr, 1> expression_array;
  ASSERT_EQ(OB_SUCCESS, expression_array.push_back(f.expr));
  ASSERT_EQ(OB_SUCCESS, f.exec.fill_px_batch_info(batch, 0, expression_array));
  ObEvalCtx ctx(f.exec);
  EXPECT_EQ(123, f.expr.locate_expr_datum(ctx).get_int());
  EXPECT_EQ(123, f.legacy().get_int());
  EXPECT_TRUE(f.expr.get_eval_info(ctx).evaluated_);
  EXPECT_EQ(OB_ERR_UNEXPECTED, f.exec.fill_px_batch_info(batch, -1, expression_array));
}
TEST(DynamicParamCapture, native_lob_header_adjustment_keeps_mapped_cell_rules) {
  Frame f;
  ObObjMeta plain;
  plain.set_type(ObTextType);
  plain.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObObjMeta header = plain;
  header.set_has_lob_header();
  ObDatum source;
  source.set_string(ObString("raw payload"));
  ObDatum output(source);
  EXPECT_EQ(OB_SUCCESS, ob_adjust_lob_datum(f.exec, source, plain, plain, f.arena, output));
  EXPECT_EQ(source.get_string(), output.get_string());
  EXPECT_EQ(OB_ERR_UNEXPECTED, ob_adjust_lob_datum(f.exec, source, plain, header, f.arena, output));
  ObDatum null;
  null.set_null();
  EXPECT_EQ(OB_SUCCESS, ob_adjust_lob_datum(f.exec, null, header, plain, f.arena, output));
  const int64_t prefix = offsetof(ObLobCommon, buffer_);
  char *storage = static_cast<char *>(f.arena.alloc(prefix + source.len_));
  new (storage) ObLobCommon();
  memcpy(storage + prefix, source.ptr_, source.len_);
  ObDatum with_header;
  with_header.set_string(storage, prefix + source.len_);
  output = with_header;
  ASSERT_EQ(OB_SUCCESS, ob_adjust_lob_datum(f.exec, with_header, header, plain, f.arena, output));
  EXPECT_EQ(ObString("raw payload"), output.get_string());
}

TEST(DynamicParamCapture, native_extend_array_frame_captures_and_restores_parameter_address) {
  Frame f;
  ObEvalCtx ctx(f.exec);
  DatumParameter leaf = f.integer(42);
  auto *array = DatumParameterArray::alloc(f.arena, 1);
  ASSERT_NE(nullptr, array);
  array->data_[0] = leaf;
  f.expr.obj_meta_.set_ext();
  f.expr.obj_meta_.set_extend_type(T_EXT_SQL_ARRAY);
  f.expr.obj_datum_map_ = OBJ_DATUM_EXTEND;
  f.legacy().set_extend(reinterpret_cast<int64_t>(array), T_EXT_SQL_ARRAY);
  f.seed_native();
  ObDatum &frame = f.expr.locate_datum_for_write(ctx);
  frame.set_extend(reinterpret_cast<int64_t>(array), T_EXT_SQL_ARRAY, 0);
  ObDynamicParamSetter setter(0, nullptr, &f.expr);
  DatumParameter captured;
  ASSERT_EQ(OB_SUCCESS, setter.capture_dynamic_param(ctx, captured));
  ASSERT_EQ(sizeof(int64_t), captured.datum_.len_);
  EXPECT_EQ(array, captured.get_array<DatumParameterArray>());
  ASSERT_EQ(OB_SUCCESS, setter.restore_dynamic_param(ctx, captured));
  EXPECT_TRUE(f.expr.locate_expr_datum(ctx).is_native_extend());
  EXPECT_EQ(reinterpret_cast<int64_t>(array), f.expr.locate_expr_datum(ctx).get_ext());
  EXPECT_EQ(reinterpret_cast<int64_t>(array), f.legacy().get_ext());
  ObBatchRescanParams batch;
  ObBatchRescanParams::Row row(4 * sizeof(DatumParameter));
  ObSEArray<int64_t, 1> ids, exprs;
  ASSERT_EQ(OB_SUCCESS, ids.push_back(0));
  ASSERT_EQ(OB_SUCCESS, exprs.push_back(1));
  ASSERT_EQ(OB_SUCCESS, row.push_back(captured));
  ASSERT_EQ(OB_SUCCESS, batch.append_batch_rescan_param(ids, row, exprs));
  auto *owned = batch.get_one_batch_params(0).at(0).get_array<DatumParameterArray>();
  ASSERT_NE(array, owned);
  ObSEArray<ObExpr, 1> expressions;
  ASSERT_EQ(OB_SUCCESS, expressions.push_back(f.expr));
  ASSERT_EQ(OB_SUCCESS, f.exec.fill_px_batch_info(batch, 0, expressions));
  EXPECT_TRUE(f.expr.locate_expr_datum(ctx).is_native_extend());
  EXPECT_EQ(reinterpret_cast<int64_t>(owned), f.expr.locate_expr_datum(ctx).get_ext());
  EXPECT_EQ(42, owned->data_[0].datum_.get_int());
}

TEST(DynamicParamCapture, native_runtime_binding_preserves_metadata_and_commits_both_views) {
  Frame f;
  DatumParameter value = f.integer(42);
  value.set_precision(19);
  value.set_raw_text_info(3, 7);
  value.set_result_flags(0xa5);
  value.get_param_flag().flag_ = 0x5a;
  value.set_param_meta(value.get_type_meta());
  auto *plan = f.exec.get_physical_plan_ctx();
  ASSERT_EQ(OB_SUCCESS, plan->set_runtime_parameter(0, value));
  EXPECT_EQ(42, f.native().datum_.get_int());
  EXPECT_EQ(42, f.legacy().get_int());
  EXPECT_EQ(value.get_accuracy(), f.native().get_accuracy());
  EXPECT_EQ(value.get_accuracy(), f.legacy().get_accuracy());
  EXPECT_EQ(value.get_param_flag().flag_, f.native().get_param_flag().flag_);
  EXPECT_EQ(value.get_result_flag(), f.legacy().get_result_flag());
  EXPECT_EQ(3, f.native().get_raw_text_pos());
  EXPECT_EQ(7, f.legacy().get_raw_text_len());
  DatumParameter invalid;
  ObObjMeta meta;
  meta.set_type(ObMaxType);
  invalid.set_type_meta(meta);
  invalid.datum_ = value.datum_;
  EXPECT_NE(OB_SUCCESS, plan->set_runtime_parameter(0, invalid));
  EXPECT_EQ(42, f.native().datum_.get_int());
  EXPECT_EQ(42, f.legacy().get_int());
  EXPECT_EQ(OB_INVALID_ARGUMENT, plan->set_runtime_parameter(-1, value));
  EXPECT_EQ(OB_INVALID_ARGUMENT, plan->set_runtime_parameter(1, value));
}
TEST(DynamicParamCapture, native_runtime_null_keeps_declared_type_and_flags) {
  Frame f;
  DatumParameter source = f.integer(77);
  source.set_param_meta(source.get_type_meta());
  source.set_precision(19);
  source.set_scale(3);
  source.set_result_flags(0xa5);
  source.get_param_flag().flag_ = 0x5a;
  ASSERT_EQ(OB_SUCCESS, f.exec.get_physical_plan_ctx()->set_runtime_parameter(0, source));
  ObObjParam expected = f.legacy();
  expected.set_null();
  source.set_null();
  ASSERT_EQ(OB_SUCCESS, f.exec.get_physical_plan_ctx()->set_runtime_parameter(0, source));
  EXPECT_TRUE(f.native().is_null());
  EXPECT_TRUE(f.legacy().is_null());
  EXPECT_EQ(expected.get_meta(), f.native().get_type_meta());
  EXPECT_EQ(expected.get_meta(), f.legacy().get_meta());
  EXPECT_EQ(expected.get_param_meta(), f.native().get_param_meta());
  EXPECT_EQ(expected.get_accuracy(), f.native().get_accuracy());
  EXPECT_EQ(expected.get_result_flag(), f.legacy().get_result_flag());
  EXPECT_EQ(expected.get_param_flag().flag_, f.native().get_param_flag().flag_);
}
TEST(DynamicParamCapture, updated_frame_and_native_store_stay_in_sync) {
  Frame f;
  ObEvalCtx ctx(f.exec);
  ObDynamicParamSetter setter(0, nullptr, &f.expr);
  DatumParameter initial = f.integer(7);
  initial.set_precision(19);
  initial.set_param_meta();
  ASSERT_EQ(OB_SUCCESS, f.exec.get_physical_plan_ctx()->set_runtime_parameter(0, initial));
  auto input = f.integer(42);
  ASSERT_EQ(OB_SUCCESS, setter.update_dynamic_param(ctx, input.datum_));
  EXPECT_EQ(42, f.native().datum_.get_int());
  EXPECT_EQ(42, f.legacy().get_int());
  EXPECT_EQ(f.expr.locate_expr_datum(ctx).ptr_, f.native().datum_.ptr_);
  DatumParameter saved;
  ASSERT_EQ(OB_SUCCESS, saved.deep_copy(f.native(), f.arena));
  input.datum_.set_int(88);
  ASSERT_EQ(OB_SUCCESS, setter.update_dynamic_param(ctx, input.datum_));
  EXPECT_EQ(42, saved.datum_.get_int());
  EXPECT_EQ(88, f.native().datum_.get_int());
  ASSERT_EQ(OB_SUCCESS, setter.restore_dynamic_param(ctx, saved));
  EXPECT_EQ(42, f.native().datum_.get_int());
  EXPECT_EQ(42, f.expr.locate_expr_datum(ctx).get_int());
}
TEST(DynamicParamCapture, owned_runtime_backup_survives_rebinding_and_restore_scope) {
  Frame f;
  ObEvalCtx ctx(f.exec);
  ObDynamicParamSetter setter(0, nullptr, &f.expr);
  DatumParameter saved;
  {
    auto input = f.integer(42);
    ASSERT_EQ(OB_SUCCESS, setter.update_dynamic_param(ctx, input.datum_));
    ASSERT_EQ(OB_SUCCESS, saved.deep_copy(f.native(), f.exec.get_allocator()));
    input.datum_.set_int(88);
    ASSERT_EQ(OB_SUCCESS, setter.update_dynamic_param(ctx, input.datum_));
    ASSERT_EQ(OB_SUCCESS, f.exec.get_physical_plan_ctx()->set_runtime_parameter(0, saved));
  }
  EXPECT_EQ(42, f.native().datum_.get_int());
  EXPECT_EQ(42, f.legacy().get_int());
  // A group range guard restores bindings without modifying SQL expression frames.
  EXPECT_EQ(88, f.expr.locate_expr_datum(ctx).get_int());
}
namespace {
std::string decode_hex_runtime(const std::string &text) {
  std::string output;
  for (size_t i = 0; i < text.size(); i += 2) {
    output += char(std::stoul(text.substr(i, 2), nullptr, 16));
  }
  return output;
}
std::string parameter_wire_runtime(const ObObjParam &value) {
  std::string output(value.get_serialize_size(), '\0');
  int64_t p = 0;
  EXPECT_EQ(OB_SUCCESS, value.serialize(&output[0], output.size(), p));
  EXPECT_EQ(output.size(), p);
  return output;
}
} // namespace
TEST(DynamicParamCapture, native_update_matches_old_runtime_algorithm_for_all_scalar_goldens) {
  std::ifstream input("unittest/sql/data/range_map.golden");
  ASSERT_TRUE(input.good());
  std::string line;
  int covered = 0;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream row(line);
    std::string name, hex;
    std::getline(row, name, '\t');
    std::getline(row, hex, '\t');
    if (name == "empty" || name == "mixed" || name == "sql-array-writer-only") {
      continue;
    }
    SCOPED_TRACE(name);
    const std::string wire = decode_hex_runtime(hex);
    int64_t p = 0, version = 0, length = 0, count = 0;
    uint32_t flags = 0;
    ASSERT_EQ(OB_SUCCESS, serialization::decode(wire.data(), wire.size(), p, version));
    ASSERT_EQ(OB_SUCCESS, serialization::decode(wire.data(), wire.size(), p, length));
    ASSERT_EQ(OB_SUCCESS, serialization::decode(wire.data(), wire.size(), p, count));
    ASSERT_EQ(1, count);
    ASSERT_EQ(OB_SUCCESS, serialization::decode(wire.data(), wire.size(), p, flags));
    ObObj literal;
    ASSERT_EQ(OB_SUCCESS, literal.deserialize(wire.data(), wire.size(), p));
    Frame f;
    ObEvalCtx ctx(f.exec);
    ObDynamicParamSetter setter(0, nullptr, &f.expr);
    f.legacy().set_int(123);
    f.legacy().set_precision(19);
    f.legacy().set_param_meta();
    f.legacy().set_result_flag(0xa5);
    f.legacy().get_param_flag().flag_ = 0x5a;
    f.legacy().set_raw_text_info(17, 23);
    f.seed_native();
    f.expr.obj_meta_ = literal.get_meta();
    f.expr.obj_datum_map_ = ObDatum::get_obj_datum_map_type(literal.get_type());
    f.expr.datum_meta_.type_ = literal.get_type();
    f.expr.datum_meta_.precision_ = 38;
    f.expr.datum_meta_.scale_ = literal.get_scale();
    // Input is owned by the query arena; no fixed test frame size restriction.
    f.expr.res_buf_off_ = 0;
    ObDatum source;
    ObObjMeta source_meta;
    ASSERT_EQ(OB_SUCCESS, copy_legacy_value(literal, source, source_meta, f.arena));
    ObObjParam expected = f.legacy();
    int old_ret = source.to_obj(expected, f.expr.obj_meta_, f.expr.obj_datum_map_);
    if (old_ret == OB_SUCCESS) {
      if (f.expr.obj_meta_.is_decimal_int()) {
        expected.set_param_meta(f.expr.obj_meta_);
        expected.set_accuracy(ObAccuracy(f.expr.datum_meta_.precision_, f.expr.datum_meta_.scale_));
      } else {
        expected.set_param_meta();
      }
    }
    const int result = setter.update_dynamic_param(ctx, source);
    EXPECT_EQ(old_ret, result);
    if (result == OB_SUCCESS) {
      EXPECT_EQ(parameter_wire_runtime(expected), parameter_wire_runtime(f.legacy()));
      ObObjParam native_view;
      ASSERT_EQ(OB_SUCCESS, materialize_legacy_parameter(f.native(), native_view));
      EXPECT_EQ(parameter_wire_runtime(expected), parameter_wire_runtime(native_view));
      EXPECT_EQ(expected.get_param_meta(), f.native().get_param_meta());
      EXPECT_EQ(expected.get_accuracy(), f.native().get_accuracy());
    }
    ++covered;
  }
  EXPECT_EQ(151, covered);
}

TEST(DynamicParamCapture, execution_slots_grow_after_input_frames_are_initialized) {
  Frame f;
  f.legacy().set_int(42);
  f.legacy().set_precision(19);
  f.legacy().set_param_meta();
  auto *plan = f.exec.get_physical_plan_ctx();
  ASSERT_EQ(OB_SUCCESS, plan->init_datum_param_store());
  const auto *input_bytes = f.native().datum_.ptr_;
  const int64_t input_frame_count = plan->get_param_frame_ptrs().count();
  ASSERT_EQ(OB_SUCCESS, plan->reserve_param_space(2049));
  EXPECT_EQ(2049, plan->get_param_store().count());
  EXPECT_EQ(2049, plan->get_datum_param_store().count());
  EXPECT_EQ(42, f.native().datum_.get_int());
  EXPECT_EQ(input_bytes, f.native().datum_.ptr_);
  EXPECT_EQ(input_frame_count, plan->get_param_frame_ptrs().count());
  EXPECT_TRUE(plan->get_datum_param_store().at(2048).is_null());
  ObEvalCtx ctx(f.exec);
  ObDynamicParamSetter setter(2048, nullptr, &f.expr);
  auto value = f.integer(77);
  ASSERT_EQ(OB_SUCCESS, setter.update_dynamic_param(ctx, value.datum_));
  EXPECT_EQ(77, plan->get_datum_param_store().at(2048).datum_.get_int());
  EXPECT_EQ(77, plan->get_param_store().at(2048).get_int());
  ASSERT_EQ(OB_SUCCESS, plan->reserve_param_space(2));
  EXPECT_EQ(2049, plan->get_datum_param_store().count());
  EXPECT_EQ(77, plan->get_datum_param_store().at(2048).datum_.get_int());
}
TEST(DynamicParamCapture, initial_legacy_inputs_are_imported_when_native_slots_are_missing) {
  Frame f;
  auto *plan = f.exec.get_physical_plan_ctx();
  plan->get_datum_param_store().reuse();
  f.legacy().set_int(42);
  f.legacy().set_precision(19);
  f.legacy().set_raw_text_info(17, 23);
  ASSERT_EQ(OB_SUCCESS, plan->reserve_param_space(5));
  ASSERT_EQ(5, plan->get_datum_param_store().count());
  EXPECT_EQ(42, f.native().datum_.get_int());
  EXPECT_EQ(19, f.native().get_precision());
  EXPECT_EQ(17, f.native().get_raw_text_pos());
  EXPECT_EQ(23, f.native().get_raw_text_len());
  for (int i = 1; i < 5; ++i) {
    EXPECT_TRUE(plan->get_datum_param_store().at(i).is_null());
  }
  EXPECT_EQ(OB_INVALID_ARGUMENT, plan->reserve_param_space(-1));
  EXPECT_EQ(5, plan->get_param_store().count());
  EXPECT_EQ(5, plan->get_datum_param_store().count());
}

TEST(DynamicParamCapture, failed_slot_import_rolls_back_counts_without_discarding_native_inputs) {
  Frame f;
  auto *plan = f.exec.get_physical_plan_ctx();
  f.legacy().set_int(42);
  f.seed_native();
  const char *prefix = f.native().datum_.ptr_;
  ObObjParam good;
  good.set_int(88);
  ASSERT_EQ(OB_SUCCESS, plan->get_param_store_for_update().push_back(good));
  ObObjParam invalid = good;
  ObObjMeta meta;
  meta.set_type(ObMaxType);
  invalid.set_meta_type(meta);
  ASSERT_EQ(OB_SUCCESS, plan->get_param_store_for_update().push_back(invalid));
  EXPECT_EQ(OB_INVALID_ARGUMENT, plan->reserve_param_space(5));
  EXPECT_EQ(3, plan->get_param_store().count());
  EXPECT_EQ(1, plan->get_datum_param_store().count());
  EXPECT_EQ(prefix, f.native().datum_.ptr_);
  EXPECT_EQ(42, f.native().datum_.get_int());
  plan->get_param_store_for_update().at(2).set_int(99);
  ASSERT_EQ(OB_SUCCESS, plan->reserve_param_space(5));
  EXPECT_EQ(88, plan->get_datum_param_store().at(1).datum_.get_int());
  EXPECT_EQ(99, plan->get_datum_param_store().at(2).datum_.get_int());
}
