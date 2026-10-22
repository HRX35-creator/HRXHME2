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

#pragma once

#include "common/datum/datum_core.h"
#include "common/datum/parameter_flags.h"

namespace oceanbase
{
namespace common
{

// A parameter value is a Datum plus independent metadata. There is no embedded
// general-purpose value object. Payloads are views: copying this descriptor
// does not extend the source arena lifetime; deep_copy() creates bytes in
// the destination arena. No payload ever points into this object's own storage.
class DatumParameter
{
public:
  DatumParameter()
    : datum_(), type_meta_(), accuracy_(), res_flags_(0), flag_(),
      raw_text_pos_(-1), raw_text_len_(-1), param_meta_()
  { datum_.set_null(); }

  void reset() { *this = DatumParameter(); }
  void set_datum(const ObDatum &datum) { datum_ = datum; }
  const ObObjMeta &get_type_meta() const { return type_meta_; }
  void set_type_meta(const ObObjMeta &meta) { type_meta_ = meta; }
  ObObjType get_type() const { return type_meta_.get_type(); }
  ObCollationType get_collation_type() const { return type_meta_.get_collation_type(); }
  ObCollationLevel get_collation_level() const { return type_meta_.get_collation_level(); }
  bool is_ext_sql_array() const { return type_meta_.is_ext_sql_array(); }
  bool is_lob() const { return type_meta_.is_lob(); }
  bool is_string_type() const { return type_meta_.is_string_type(); }
  bool is_null() const { return datum_.is_null(); }
  bool has_lob_header() const { return type_meta_.has_lob_header(); }

  void set_accuracy(const ObAccuracy &accuracy) { accuracy_ = accuracy; }
  const ObAccuracy &get_accuracy() const { return accuracy_; }
  void set_length(ObLength length) { accuracy_.set_length(length); }
  void set_precision(ObPrecision precision) { accuracy_.set_precision(precision); }
  void set_length_semantics(ObLengthSemantics semantics)
  { accuracy_.set_length_semantics(semantics); }
  void set_scale(ObScale scale) { accuracy_.set_scale(scale); type_meta_.set_scale(scale); }
  void set_udt_id(uint64_t id) { accuracy_.set_accuracy(id); }
  ObLength get_length() const { return accuracy_.get_length(); }
  ObPrecision get_precision() const { return accuracy_.get_precision(); }
  ObScale get_scale() const { return accuracy_.get_scale(); }
  uint64_t get_udt_id() const
  { return type_meta_.is_ext() ? accuracy_.get_accuracy() : OB_INVALID_INDEX; }

  void set_result_flags(uint32_t flags) { res_flags_ = flags; }
  void set_result_flag(uint32_t flags) { res_flags_ |= flags; }
  void unset_result_flag(uint32_t flags) { res_flags_ &= ~flags; }
  bool has_result_flag(uint32_t flags) const { return (res_flags_ & flags) != 0; }
  uint32_t get_result_flag() const { return res_flags_; }
  const ParamFlag &get_param_flag() const { return flag_; }
  ParamFlag &get_param_flag() { return flag_; }
  void set_param_flag(const ParamFlag &flag) { flag_ = flag; }
  void set_need_to_check_type(bool value) { flag_.need_to_check_type_ = value; }
  bool need_to_check_type() const { return flag_.need_to_check_type_; }
  void set_need_to_check_bool_value(bool value) { flag_.need_to_check_bool_value_ = value; }
  bool need_to_check_bool_value() const { return flag_.need_to_check_bool_value_; }
  void set_expected_bool_value(bool value) { flag_.expected_bool_value_ = value; }
  bool expected_bool_value() const { return flag_.expected_bool_value_; }
  bool is_nop_value() const { return datum_.is_nop(); }

  void set_raw_text_info(int32_t pos, int32_t len)
  { raw_text_pos_ = pos; raw_text_len_ = len; }
  int32_t get_raw_text_pos() const { return raw_text_pos_; }
  int32_t get_raw_text_len() const { return raw_text_len_; }
  // Initialize declared type and numeric defaults after a parameter crosses a
  // wire/protocol boundary. A typed NULL keeps its existing declared metadata.
  void set_param_meta()
  {
    if (param_meta_.is_null() || get_type() != ObNullType) { param_meta_ = type_meta_; }
    if (ob_is_numeric_type(get_type())) {
      const ObAccuracy &defaults = ObAccuracy::DDL_DEFAULT_ACCURACY[get_type()];
      if (get_scale() < 0) {
        if (type_meta_.get_scale() >= 0) { set_scale(type_meta_.get_scale()); }
        else if (ob_is_integer_type(get_type())) { set_scale(defaults.get_scale()); }
      }
      if (get_precision() == PRECISION_UNKNOWN_YET) { set_precision(defaults.get_precision()); }
    }
  }
  void set_param_meta(const ObObjMeta &meta) { param_meta_ = meta; }
  const ObObjMeta &get_param_meta() const { return param_meta_; }

  // SQL array descriptors use an ordinary 8-byte address Datum. The object
  // behind the address belongs to its producer; copying does not clone it.
  template <typename T> void set_array_address(const T *array)
  { datum_.set_int(reinterpret_cast<int64_t>(array)); }
  template <typename T> const T *get_array() const
  {
    return is_ext_sql_array() && !is_null()
        ? reinterpret_cast<const T *>(datum_.get_int()) : nullptr;
  }
  template <typename T> T *get_array()
  {
    return is_ext_sql_array() && !is_null()
        ? reinterpret_cast<T *>(datum_.get_int()) : nullptr;
  }

  int alloc_datum_reserved_buff(const ObObjMeta &meta, int16_t precision,
                                ObIAllocator &allocator)
  {
    int ret = OB_SUCCESS;
    const ObObjType type = meta.is_ext_sql_array() ? ObIntType : meta.get_type();
    const ObObjDatumMapType mapping = ObDatum::get_obj_datum_map_type(type);
    if (mapping != OBJ_DATUM_NULL) {
      const uint32_t size = ObDatum::get_reserved_size(mapping, precision);
      if (OB_ISNULL(datum_.ptr_ = static_cast<char *>(allocator.alloc(size)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    return ret;
  }

  // Clones payload bytes, not the pointee of an extended PL/SQL array address.
  // The destination is unchanged if allocation fails, including self-copy.
  int deep_copy(const DatumParameter &source, ObIAllocator &allocator)
  {
    int ret = OB_SUCCESS;
    ObDatum copy;
    if (OB_FAIL(copy.deep_copy(source.datum_, allocator))) {
    } else {
      *this = source;
      datum_ = copy;
    }
    return ret;
  }

  static uint32_t accuracy_offset_bits() { return offsetof(DatumParameter, accuracy_) * 8; }
  static uint32_t res_flags_offset_bits() { return offsetof(DatumParameter, res_flags_) * 8; }
  static uint32_t flag_offset_bits() { return offsetof(DatumParameter, flag_) * 8; }
  TO_STRING_KV(K_(datum), K_(type_meta), K_(accuracy), K_(res_flags), K_(flag),
                K_(raw_text_pos), K_(raw_text_len), K_(param_meta));

  ObDatum datum_;
private:
  ObObjMeta type_meta_;
  ObAccuracy accuracy_;
  uint32_t res_flags_;
  ParamFlag flag_;
  int32_t raw_text_pos_;
  int32_t raw_text_len_;
  ObObjMeta param_meta_;
};

} // namespace common
} // namespace oceanbase
