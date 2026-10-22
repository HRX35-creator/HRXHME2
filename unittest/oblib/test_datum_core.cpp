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

#include "common/datum/datum_core.h"
#include "common/datum/datum_parameter.h"
#include "common/datum/datum_param_store.h"

#ifdef OCEANBASE_COMMON_OB_OBJECT_H_
#error "Native Datum headers must not include the legacy value definition"
#endif

#include <cstring>
#include <type_traits>
#include <gtest/gtest.h>
#include "lib/allocator/ob_allocator.h"

using namespace oceanbase::common;

namespace {
template <typename T, typename = void> struct Complete : std::false_type {};
template <typename T> struct Complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};
static_assert(!Complete<ObObj>::value, "Legacy values must stay incomplete in native-only code");
static_assert(sizeof(ObDatum) == 12, "The Datum slot ABI must stay unchanged");
static_assert(sizeof(ObObjMeta) == 4, "The metadata ABI must stay unchanged");
}

TEST(DatumCore, scalar_parameter_and_copy_need_no_legacy_definition)
{
  ObArenaAllocator arena;
  DatumParameter value, copy;
  ObObjMeta meta; meta.set_uint64(); value.set_type_meta(meta);
  ASSERT_EQ(OB_SUCCESS, value.alloc_datum_reserved_buff(meta, -1, arena));
  value.datum_.set_uint(UINT64_MAX);
  ASSERT_EQ(OB_SUCCESS, copy.deep_copy(value, arena));
  EXPECT_EQ(UINT64_MAX, copy.datum_.get_uint());
  EXPECT_NE(copy.datum_.ptr_, value.datum_.ptr_);
  copy.datum_.set_null(); EXPECT_TRUE(copy.is_null());
}

TEST(DatumCore, native_store_growth_preserves_payloads)
{
  ObArenaAllocator arena;
  DatumParamStore store{ObWrapperAllocator(arena)};
  const int64_t count = store.get_block_size() / sizeof(DatumParameter) + 17;
  ObObjMeta meta;
  meta.set_int();
  for (int64_t i = 0; i < count; ++i) {
    DatumParameter value;
    value.set_type_meta(meta);
    value.set_precision(19);
    ASSERT_EQ(OB_SUCCESS, value.alloc_datum_reserved_buff(meta, 19, arena));
    value.datum_.set_int(i * 7);
    ASSERT_EQ(OB_SUCCESS, store.push_back(value));
  }
  ASSERT_EQ(count, store.count());
  EXPECT_GT(store.mem_used(), store.get_block_size());
  for (int64_t i = 0; i < count; ++i) {
    EXPECT_EQ(i * 7, store.at(i).datum_.get_int());
    EXPECT_EQ(ObIntType, store.at(i).get_type());
    EXPECT_EQ(19, store.at(i).get_precision());
  }
}

TEST(DatumCore, metadata_keeps_legacy_four_byte_order_and_all_attributes)
{
  ObObjMeta meta; meta.set_varchar();
  meta.set_collation_level(CS_LEVEL_COERCIBLE);
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN); meta.set_scale(6);
  const unsigned char expected[] = {22, 4, 46, 6};
  char wire[64]; int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, meta.serialize(wire, sizeof(wire), pos));
  ASSERT_EQ(sizeof(expected), pos); EXPECT_EQ(pos, meta.get_serialize_size());
  EXPECT_EQ(0, memcmp(expected, wire, pos));
  ObObjMeta decoded; int64_t read = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(wire, pos, read));
  EXPECT_EQ(6, decoded.get_scale());
  EXPECT_EQ(meta.get_collation_type(), decoded.get_collation_type());
  EXPECT_EQ(meta.get_collation_level(), decoded.get_collation_level());
  meta.set_collection(0xfedc); pos = 0;
  ASSERT_EQ(OB_SUCCESS, meta.serialize(wire, sizeof(wire), pos)); read = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(wire, pos, read));
  EXPECT_EQ(0xfedc, decoded.get_subschema_id());
}

TEST(DatumCore, parameter_flags_keep_version_one_wire_and_runtime_extension)
{
  ParamFlag flags; flags.flag_ = 0xa5; flags.extend_flag_ = 0x5a;
  const unsigned char expected[] = {1, 0x81, 0x80, 0x80, 0x80, 0, 0xa5};
  char wire[64]; int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, flags.serialize(wire, sizeof(wire), pos));
  ASSERT_EQ(sizeof(expected), pos); EXPECT_EQ(pos, flags.get_serialize_size());
  EXPECT_EQ(0, memcmp(wire, expected, pos));
  ParamFlag decoded; int64_t read = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(wire, pos, read));
  EXPECT_EQ(0xa5, decoded.flag_);
  EXPECT_EQ(0, decoded.extend_flag_); // Runtime extension is absent from version 1.
  ParamFlag copy(flags); EXPECT_EQ(0x5a, copy.extend_flag_);
}

TEST(DatumCore, native_parameter_reports_real_nop_sentinel)
{
  char bytes[DATUM_LEGACY_EXTEND_SIZE]; DatumParameter value;
  ObObjMeta meta; meta.set_ext(); value.set_type_meta(meta);
  value.datum_.ptr_ = bytes; value.datum_.set_ext_value(ObActionFlag::OP_NOP);
  EXPECT_TRUE(value.is_nop_value());
  value.datum_.set_ext_value(DATUM_MIN_EXT_VALUE); EXPECT_FALSE(value.is_nop_value());
  value.datum_.set_null(); EXPECT_FALSE(value.is_nop_value());
}

TEST(DatumCore, enumset_payload_decodes_without_value_bridge)
{
  ObString text("中文"); ObEnumSetInnerValue source(42, text), decoded;
  char wire[128]; int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize(wire, sizeof(wire), pos));
  ObDatum value; value.set_enumset_inner(wire, pos);
  ASSERT_EQ(OB_SUCCESS, value.get_enumset_inner(decoded));
  EXPECT_EQ(42, decoded.numberic_value_); EXPECT_EQ(text, decoded.string_value_);
}

TEST(DatumCore, lob_views_keep_borrowed_locator_and_storage_header)
{
  ObMemLobCommon memory_header(TEMP_FULL_LOB, true);
  ObDatum value;
  value.set_string(reinterpret_cast<const char *>(&memory_header), sizeof(memory_header));
  value.set_has_lob_header();
  ObLobLocatorV2 locator;
  value.get_mem_lob(locator);
  EXPECT_EQ(reinterpret_cast<char *>(&memory_header), locator.ptr_);
  EXPECT_EQ(sizeof(memory_header), locator.size_);
  EXPECT_TRUE(locator.has_lob_header_);
  EXPECT_TRUE(locator.is_valid(false));
  ObLobCommon storage_header;
  value.set_lob_data(storage_header, sizeof(storage_header));
  EXPECT_EQ(&storage_header, &value.get_lob_data());
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
