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

#include "common/datum/datum_parameter_codec.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "sql/rewrite/ob_query_range_define.h"
#include "sql/rewrite/ob_range_generator.h"
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace {
struct RangeFrame {
  std::string name, wire, canonical;
};
std::string unhex(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i += 2) {
    out += char(std::stoul(s.substr(i, 2), nullptr, 16));
  }
  return out;
}
std::vector<RangeFrame> frames() {
  std::ifstream file("unittest/sql/data/range_map.golden");
  EXPECT_TRUE(file.good());
  std::vector<RangeFrame> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream row(line);
    std::string name, wire, canonical;
    std::getline(row, name, '\t');
    std::getline(row, wire, '\t');
    std::getline(row, canonical);
    out.push_back({name, unhex(wire), unhex(canonical)});
  }
  return out;
}
std::string encode(const ObRangeMap &map) {
  std::string out(map.get_serialize_size(), '\0');
  int64_t p = 0;
  EXPECT_EQ(OB_SUCCESS, map.serialize(&out[0], out.size(), p));
  EXPECT_EQ(out.size(), p);
  return out;
}
std::string wire_body(const std::string &body) {
  std::string out(1 + serialization::OB_SERIALIZE_SIZE_NEED_BYTES + body.size(), '\0');
  int64_t p = 0;
  EXPECT_EQ(OB_SUCCESS, serialization::encode_vi64(&out[0], out.size(), p, 1));
  EXPECT_EQ(OB_SUCCESS, serialization::encode_fixed_bytes_i64(&out[0], out.size(), p, body.size()));
  memcpy(&out[p], body.data(), body.size());
  return out;
}
std::string vi64(int64_t n) {
  char b[16];
  int64_t p = 0;
  EXPECT_EQ(OB_SUCCESS, serialization::encode_vi64(b, sizeof(b), p, n));
  return std::string(b, p);
}
std::string bare_value(const std::string &wire) {
  int64_t p = 0, v = 0, n = 0;
  EXPECT_EQ(OB_SUCCESS, serialization::decode(wire.data(), wire.size(), p, v));
  EXPECT_EQ(OB_SUCCESS, serialization::decode(wire.data(), wire.size(), p, n));
  EXPECT_EQ(1, static_cast<unsigned char>(wire[p]));
  ++p;
  ++p;
  return wire.substr(p, wire.size() - p - 1);
}
class CountingAllocator : public ObIAllocator {
public:
  ObArenaAllocator arena;
  int64_t calls = 0, bytes = 0;
  bool reject = false;
  void *alloc(int64_t n) override {
    ++calls;
    bytes += n;
    return reject ? nullptr : arena.alloc(n);
  }
  void *alloc(int64_t n, const ObMemAttr &) override { return alloc(n); }
  void free(void *) override {}
};
} // namespace
TEST(RangeMapDatum, constants_are_datums_with_metadata_in_existing_padding) {
  static_assert(std::is_same<decltype(ObRangeMap::ExprFinalInfo::const_val_), ObDatum *>::value,
                "No legacy value in range constants");
  EXPECT_EQ(24, sizeof(ObRangeMap::ExprFinalInfo));
  EXPECT_EQ(4, offsetof(ObRangeMap::ExprFinalInfo, const_meta_));
  auto corpus = frames();
  ASSERT_EQ(154, corpus.size());
  for (auto f : corpus) {
    SCOPED_TRACE(f.name);
    ObArenaAllocator arena;
    ObRangeMap map(arena);
    int64_t p = 0;
    ASSERT_EQ(OB_SUCCESS, map.deserialize(f.wire.data(), f.wire.size(), p));
    EXPECT_EQ(f.wire.size(), p);
    EXPECT_EQ(f.canonical, encode(map));
    memset(&f.wire[0], 0xa5, f.wire.size());
    EXPECT_EQ(f.canonical, encode(map));
    if (f.name == "mixed") {
      ASSERT_EQ(3, map.expr_final_infos_.count());
      EXPECT_TRUE(map.expr_final_infos_.at(0).is_param_);
      EXPECT_EQ(17, map.expr_final_infos_.at(0).param_idx_);
      EXPECT_EQ(ObVarcharType, map.expr_final_infos_.at(1).const_meta_.get_type());
      EXPECT_TRUE(map.expr_final_infos_.at(2).is_expr_);
      ASSERT_EQ(1, map.in_params_.count());
      ASSERT_EQ(3, map.in_params_.at(0)->count());
      EXPECT_EQ(-3, map.in_params_.at(0)->at(2));
    }
  }
}
TEST(RangeMapDatum, deep_copy_of_constants_and_arrays_outlives_source_arena) {
  for (const auto &f : frames()) {
    if (f.name == "empty" || f.name == "mixed") {
      continue;
    }
    SCOPED_TRACE(f.name);
    ObArenaAllocator output_arena;
    ObPreRangeGraph copied(output_arena);
    {
      ObArenaAllocator source_arena;
      ObRangeMap source(source_arena);
      int64_t p = 0;
      ASSERT_EQ(OB_SUCCESS, source.deserialize(f.wire.data(), f.wire.size(), p));
      ASSERT_EQ(OB_SUCCESS, copied.deep_copy_range_map(source));
    }
    EXPECT_EQ(f.canonical, encode(copied.get_range_map()));
  }
}
TEST(RangeMapDatum, truncated_and_invalid_records_never_expose_partial_maps) {
  for (const auto &f : frames()) {
    SCOPED_TRACE(f.name);
    for (size_t n = 0; n < f.wire.size(); ++n) {
      ObArenaAllocator a;
      ObRangeMap map(a);
      int64_t p = 0;
      EXPECT_NE(OB_SUCCESS, map.deserialize(f.wire.data(), n, p));
      EXPECT_EQ(0, map.expr_final_infos_.count());
      EXPECT_EQ(0, map.in_params_.count());
    }
  }
  std::vector<std::string> bodies = {vi64(-1),
                                     vi64(INT64_MAX),
                                     std::string("\1\2", 2),
                                     std::string("\1\1", 2),
                                     std::string("\1\4", 2),
                                     std::string("\0", 1) + vi64(-1),
                                     std::string("\0", 1) + vi64(INT64_MAX),
                                     std::string("\0\1", 2) + vi64(INT64_MAX)};
  for (const auto &body : bodies) {
    ObArenaAllocator a;
    ObRangeMap map(a);
    std::string wire = wire_body(body);
    int64_t p = 0;
    EXPECT_NE(OB_SUCCESS, map.deserialize(wire.data(), wire.size(), p));
    EXPECT_EQ(0, map.expr_final_infos_.count());
    EXPECT_EQ(0, map.in_params_.count());
  }
}
TEST(RangeMapDatum, reused_decoder_replaces_in_lists_and_clears_failed_constants) {
  auto corpus = frames();
  const RangeFrame *mixed = nullptr, *empty = nullptr;
  for (const auto &f : corpus) {
    if (f.name == "mixed") {
      mixed = &f;
    }
    if (f.name == "empty") {
      empty = &f;
    }
  }
  ASSERT_NE(nullptr, mixed);
  ASSERT_NE(nullptr, empty);
  ObArenaAllocator arena;
  ObRangeMap map(arena);
  int64_t p = 0;
  ASSERT_EQ(OB_SUCCESS, map.deserialize(mixed->wire.data(), mixed->wire.size(), p));
  ASSERT_EQ(1, map.in_params_.count());
  p = 0;
  ASSERT_EQ(OB_SUCCESS, map.deserialize(empty->wire.data(), empty->wire.size(), p));
  EXPECT_EQ(0, map.in_params_.count());
  EXPECT_EQ(0, map.expr_final_infos_.count());
  const std::string bad = wire_body(std::string("\1\2", 2));
  p = 0;
  EXPECT_NE(OB_SUCCESS, map.deserialize(bad.data(), bad.size(), p));
  EXPECT_EQ(0, map.expr_final_infos_.count());
  EXPECT_EQ(0, map.in_params_.count());
}
TEST(RangeMapDatum, native_constants_materialize_only_at_old_range_boundary) {
  int covered = 0;
  for (const auto &f : frames()) {
    if (f.name != "value-type-0" && f.name != "value-type-5" && f.name != "value-type-10" &&
        f.name != "value-type-15" && f.name != "value-type-17" && f.name != "value-type-22" &&
        f.name != "value-decimal-width-4" && f.name != "sql-array-writer-only") {
      continue;
    }
    SCOPED_TRACE(f.name);
    ObArenaAllocator a;
    ObRangeMap map(a);
    ObExecContext ctx(a);
    ASSERT_EQ(OB_SUCCESS, ctx.create_physical_plan_ctx());
    int64_t p = 0;
    ASSERT_EQ(OB_SUCCESS, map.deserialize(f.wire.data(), f.wire.size(), p));
    ObObj value;
    bool valid = false;
    ASSERT_EQ(OB_SUCCESS, ObRangeGenerator::calc_result_value(a, map, 0, value, valid, ctx));
    EXPECT_TRUE(valid);
    std::string output(value.get_serialize_size(), '\0');
    p = 0;
    ASSERT_EQ(OB_SUCCESS, value.serialize(&output[0], output.size(), p));
    EXPECT_EQ(bare_value(f.wire), output);
    ++covered;
  }
  EXPECT_EQ(8, covered);
}
TEST(RangeMapDatum, copying_values_uses_one_owned_payload_without_unused_reserve) {
  CountingAllocator strings;
  char bytes[] = "abc";
  ObObjParam source;
  source.set_varchar(ObString(3, bytes));
  source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  source.set_result_flag(0x1234);
  source.set_raw_text_info(17, 19);
  DatumParameter value;
  ASSERT_EQ(OB_SUCCESS, copy_legacy_parameter(source, value, strings));
  EXPECT_EQ(1, strings.calls);
  EXPECT_EQ(3, strings.bytes);
  memset(bytes, 'x', 3);
  EXPECT_EQ(ObString("abc"), value.datum_.get_string());
  EXPECT_EQ(0x1234, value.get_result_flag());
  EXPECT_EQ(17, value.get_raw_text_pos());
  EXPECT_EQ(19, value.get_raw_text_len());
  CountingAllocator decimals;
  int32_t raw = -1234;
  source.reset();
  source.set_decimal_int(sizeof(raw), 2, reinterpret_cast<ObDecimalInt *>(&raw));
  source.set_precision(38);
  ASSERT_EQ(OB_SUCCESS, copy_legacy_parameter(source, value, decimals));
  EXPECT_EQ(1, decimals.calls);
  EXPECT_EQ(sizeof(raw), decimals.bytes);
  raw = 0;
  EXPECT_EQ(-1234, *reinterpret_cast<const int32_t *>(value.datum_.ptr_));
}
TEST(RangeMapDatum, failed_value_clone_leaves_datum_and_type_unchanged) {
  CountingAllocator rejecting;
  rejecting.reject = true;
  ObObj object;
  object.set_varchar("payload");
  ObDatum destination;
  int64_t scalar = 77;
  destination.set_string(reinterpret_cast<const char *>(&scalar), sizeof(scalar));
  ObObjMeta meta;
  meta.set_int();
  const ObDatum before = destination;
  const ObObjMeta before_meta = meta;
  EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, copy_legacy_value(object, destination, meta, rejecting));
  EXPECT_EQ(before.ptr_, destination.ptr_);
  EXPECT_EQ(before.pack_, destination.pack_);
  EXPECT_EQ(before_meta, meta);
}
TEST(RangeMapDatum, fast_nlj_constant_bounds_survive_plan_arena_and_keep_null_rules) {
  ObArenaAllocator output_arena;
  char *memory = static_cast<char *>(output_arena.alloc(sizeof(ObNewRange) + 2 * sizeof(ObObj)));
  ASSERT_NE(nullptr, memory);
  auto *range = new (memory) ObNewRange();
  new (memory + sizeof(ObNewRange)) ObObj();
  new (memory + sizeof(ObNewRange) + sizeof(ObObj)) ObObj();
  {
    ObArenaAllocator plan_arena;
    ObPreRangeGraph graph(plan_arena);
    ObRangeNode node(plan_arena);
    int64_t key = 0;
    node.column_cnt_ = 1;
    node.start_keys_ = &key;
    node.end_keys_ = &key;
    node.include_start_ = true;
    node.include_end_ = true;
    graph.set_range_head(&node);
    ObObj input;
    input.set_varchar("lookup key");
    input.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ObDatum value;
    ObObjMeta meta;
    ASSERT_EQ(OB_SUCCESS, copy_legacy_value(input, value, meta, plan_arena));
    ASSERT_EQ(OB_SUCCESS, graph.get_range_map().expr_final_infos_.prepare_allocate(1));
    auto &info = graph.get_range_map().expr_final_infos_.at(0);
    info.is_const_ = true;
    info.const_val_ = &value;
    info.const_meta_ = meta;
    ObRangeColumnMeta column;
    column.column_type_.set_type(ObVarcharType);
    column.column_type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    column.column_type_.set_collation_level(meta.get_collation_level());
    ObSEArray<ObRangeColumnMeta *, 1> columns;
    ASSERT_EQ(OB_SUCCESS, columns.push_back(&column));
    ASSERT_EQ(OB_SUCCESS, graph.deep_copy_column_metas(columns));
    ParamStore no_parameters;
    bool valid = false;
    ASSERT_EQ(OB_SUCCESS,
              ObRangeGenerator::check_can_final_fast_nlj_range(graph, no_parameters, valid));
    EXPECT_TRUE(valid);
    ASSERT_EQ(OB_SUCCESS,
              ObRangeGenerator::generate_fast_nlj_range(graph, no_parameters, output_arena, range));
  }
  EXPECT_EQ(ObString("lookup key"), range->start_key_.get_obj_ptr()[0].get_string());
  EXPECT_EQ(ObString("lookup key"), range->end_key_.get_obj_ptr()[0].get_string());
  ObArenaAllocator a;
  ObPreRangeGraph graph(a);
  ObRangeNode node(a);
  int64_t key = 0;
  node.column_cnt_ = 1;
  node.start_keys_ = &key;
  node.end_keys_ = &key;
  graph.set_range_head(&node);
  ASSERT_EQ(OB_SUCCESS, graph.get_range_map().expr_final_infos_.prepare_allocate(1));
  auto &info = graph.get_range_map().expr_final_infos_.at(0);
  ObDatum null;
  null.set_null();
  info.is_const_ = true;
  info.const_val_ = &null;
  info.const_meta_.set_null();
  ParamStore empty;
  ASSERT_EQ(OB_SUCCESS, ObRangeGenerator::generate_fast_nlj_range(graph, empty, a, range));
  EXPECT_TRUE(range->start_key_.get_obj_ptr()[0].is_max_value());
  EXPECT_TRUE(range->end_key_.get_obj_ptr()[0].is_min_value());
  info.null_safe_ = true;
  ASSERT_EQ(OB_SUCCESS, ObRangeGenerator::generate_fast_nlj_range(graph, empty, a, range));
  EXPECT_TRUE(range->start_key_.get_obj_ptr()[0].is_null());
  EXPECT_TRUE(range->end_key_.get_obj_ptr()[0].is_null());
}
TEST(RangeMapDatum, legacy_value_import_preserves_every_captured_value_and_owns_payload) {
  int count = 0;
  for (const auto &f : frames()) {
    if (f.name == "empty" || f.name == "mixed") {
      continue;
    }
    SCOPED_TRACE(f.name);
    ObArenaAllocator destination_arena;
    ObDatum copied;
    ObObjMeta meta;
    uint32_t flags = 0;
    {
      ObArenaAllocator source_arena;
      ObRangeMap source(source_arena);
      int64_t p = 0;
      ASSERT_EQ(OB_SUCCESS, source.deserialize(f.wire.data(), f.wire.size(), p));
      const auto &entry = source.expr_final_infos_.at(0);
      flags = entry.flags_;
      ObObj old_view;
      ASSERT_EQ(OB_SUCCESS,
                materialize_legacy_value(*entry.const_val_, entry.const_meta_, old_view));
      ASSERT_EQ(OB_SUCCESS, copy_legacy_value(old_view, copied, meta, destination_arena));
    }
    ObRangeMap output(destination_arena);
    ASSERT_EQ(OB_SUCCESS, output.expr_final_infos_.prepare_allocate(1));
    auto &entry = output.expr_final_infos_.at(0);
    entry.flags_ = flags;
    entry.const_val_ = &copied;
    entry.const_meta_ = meta;
    EXPECT_EQ(f.canonical, encode(output));
    ++count;
  }
  EXPECT_EQ(152, count);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
