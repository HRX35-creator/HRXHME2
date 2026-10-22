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

#ifndef OCEANBASE_COMMON_OB_OBJECT_H_
#define OCEANBASE_COMMON_OB_OBJECT_H_

#include "lib/ob_define.h"
#include "common/object/ob_type_metadata.h"
#include "common/enumset/enumset_value.h"
#include "common/lob/lob_types.h"
#include "common/datum/parameter_flags.h"
#include "common/timezone/ob_time_def.h"
#include "lib/string/ob_string.h"
#include "common/ob_action_flag.h"
#include "common/object/ob_obj_type.h"
#include "common/ob_accuracy.h"
#include "lib/checksum/ob_crc64.h"
#include "common/number/ob_number_v2.h"
#include "lib/charset/ob_charset.h"
#include "common/timezone/ob_timezone_info.h"
#include "lib/hash/ob_hashutils.h"
#include "lib/hash_func/ob_hash_func.h"
#include "lib/charset/ob_dtoa.h"
#include "common/wide_integer/ob_wide_integer.h"

namespace oceanbase
{
namespace tests
{
namespace common
{
class ObjTest;
class ObSqlString;
}
}
namespace sql
{
class ObExecContext;
}
namespace common
{

struct ObCompareCtx;
class ObSqlCollectionInfo;
enum class ObDocIDType : uint16_t
{
  INVALID = 0,
  TABLET_SEQUENCE,
  HIDDEN_INC_PK,
};

class ObDocId final
{
public:
  ObDocId();
  ObDocId(const uint64_t tablet_id, const uint64_t seq_id);
  ~ObDocId() = default;

  void reset();
  bool is_valid() const;
  int hash(uint64_t &hash_val) const;
  ObString get_string() const;
  int from_string(const ObString &doc_id);

  bool operator ==(const ObDocId &other) const;
  bool operator !=(const ObDocId &other) const;

  TO_STRING_KV(K_(tablet_id), K_(seq_id));
public:
  static const int64_t OB_DOC_ID_HASH = 10;
  uint64_t tablet_id_;
  uint64_t seq_id_;
};

class ObCenterId final
{
public:
  ObCenterId();
  ObCenterId(const uint64_t tablet_id, const uint64_t center_id);
  ~ObCenterId() = default;

  bool is_valid() const;


  TO_STRING_KV(K_(tablet_id), K_(center_id));
public:
  uint64_t tablet_id_;
  uint64_t center_id_;
};

class ObPqCenterId final
{
public:
  ObPqCenterId();
  ObPqCenterId(const uint64_t tablet_id, const uint32_t m_id, const uint32_t center_id);
  ~ObPqCenterId() = default;

  bool is_valid() const;


  TO_STRING_KV(K_(tablet_id), K_(center_id), K_(m_id));
public:
  uint64_t tablet_id_;
  uint32_t m_id_;
  uint32_t center_id_;
};

struct ObObjPrintParams
{
  ObObjPrintParams (const ObTimeZoneInfo *tz_info, ObCollationType cs_type):
    tz_info_(tz_info),
    cs_type_(cs_type),
    accuracy_(),
    print_flags_(0),
    exec_ctx_(NULL),
    ob_obj_type_(ObNullType),
    coll_meta_(NULL)
  {}
  ObObjPrintParams (const ObTimeZoneInfo *tz_info):
    tz_info_(tz_info),
    cs_type_(CS_TYPE_UTF8MB4_GENERAL_CI),
    accuracy_(),
    print_flags_(0),
    exec_ctx_(NULL),
    ob_obj_type_(ObNullType),
    coll_meta_(NULL)
  {}
  ObObjPrintParams ():
    tz_info_(NULL),
    cs_type_(CS_TYPE_UTF8MB4_GENERAL_CI),
    accuracy_(),
    print_flags_(0),
    exec_ctx_(NULL),
    ob_obj_type_(ObNullType),
    coll_meta_(NULL)
  {}
  TO_STRING_KV(K_(tz_info), K_(cs_type),K_(print_flags), K_(ob_obj_type));
  const ObTimeZoneInfo *tz_info_;
  ObCollationType cs_type_;
  ObAccuracy accuracy_;
  union {
    uint32_t print_flags_;
    struct {
      uint32_t need_cast_expr_:1;
      uint32_t print_origin_stmt_:1;
      uint32_t use_memcpy_:1;
      uint32_t skip_escape_:1;
      uint32_t beginning_space_:1;
      uint32_t binary_string_print_hex_:1;
      uint32_t need_print_converter_:1;
      uint32_t print_const_expr_type_:1;
      uint32_t print_null_string_value_:1;
      uint32_t refine_range_max_value_:1;
      uint32_t character_hex_safe_represent_:1;
      uint32_t binary_string_print_base64_:1;
      uint32_t reserved_:21;
    };
  };

  /**
   * MySQL only
   * The limit expr may no longer be a constant number after transformer.
   * So, set exec_ctx_ if the stmt is rewritten. 
   * If exec_ctx_ is not null, Printer will calculate the value of limit expr and output.
   *  e.g. "limit (1 + 1)" will be output as "limit 2" 
   * 
  */
  sql::ObExecContext *exec_ctx_;
  ObObjType ob_obj_type_;
  common::ObSqlCollectionInfo *coll_meta_;
};

// sizeof(ObObjValue)=8
union ObObjValue
{
  int64_t int64_;
  uint64_t uint64_;

  float float_;
  double double_;

  const char *string_;

  uint32_t *nmb_digits_;

  int64_t datetime_;
  int32_t date_;
  int64_t time_;
  uint8_t year_;

  int64_t ext_;
  int64_t unknown_;
  const ObLobCommon *lob_;
  const ObMemLobCommon *lob_locator_v2_;
  int64_t nmonth_; //for interval year to month
  int64_t nsecond_; //for interval day to second

  ObDecimalInt *decimal_int_;
  void *ptr_;
};

extern int (*serialize_composite_callback)(const ObObj&, char*, const int64_t, int64_t&);
extern int (*deserialize_composite_callback)(ObObj&, const char*, const int64_t, int64_t&);
extern int64_t (*composite_serialize_size_callback)(const ObObj&);

class ObBatchChecksum;
class ObObj
{
public:
  // min, max extend value
  static const int64_t MIN_OBJECT_VALUE         = UINT64_MAX - 2;
  static const int64_t MAX_OBJECT_VALUE         = UINT64_MAX - 1;
  // WARNING: (xiaochu.yh) used only in RootTable, other user should not use this
  // to represent a (min, max) range in Roottable impl, we need this
  static const char *MIN_OBJECT_VALUE_STR;
  static const char *MAX_OBJECT_VALUE_STR;
  static const char *NOP_VALUE_STR;
public:
  ObObj();
  ObObj(const ObObj &other);
  explicit ObObj(bool val);
  explicit ObObj(int32_t val);
  explicit ObObj(int64_t val);
  explicit ObObj(ObObjType type);
  inline void reset();
  //when in not strict sql mode, build default value refer to data type
  // @string_cs_type: default value of string type need set collation type, since the space will be trim when compaction, see ObStorageSchema::trim
  int build_not_strict_default_value(int16_t precision, const ObCollationType string_cs_type);
  static ObObj make_min_obj();
  static ObObj make_max_obj();
  static ObObj make_nop_obj();
  static bool is_ext_val(int64_t v)
  {
    return (ObActionFlag::OP_MIN_OBJ <= v
        && ObActionFlag::OP_LOCK_ROW >= v)
        || MIN_OBJECT_VALUE == v
        || MAX_OBJECT_VALUE == v;
  }

  OB_INLINE void copy_meta_type(const ObObjMeta &meta) {
    meta_.set_type_simple(meta.get_type());
    meta_.set_collation_type(meta.get_collation_type());
    if (ObCharType == get_type() || ObVarcharType == get_type() || ob_is_text_tc(get_type())
        || ob_is_json(get_type()) || ob_is_geometry(get_type())) {
      meta_.set_collation_level(ObCollationLevel::CS_LEVEL_IMPLICIT);
    } else {
      meta_.set_collation_level(meta.get_collation_level());
    }
    if (meta.has_lob_header() && oceanbase::is_lob_storage(get_type())) {
      set_has_lob_header();
    }
  }

  OB_INLINE void copy_value_to(ObObj &obj, bool &has_null) const {
    switch (get_type()) {
      case ObNullType:
      case ObExtendType: {
        obj = *this;
        has_null = true;
        break;
      }
      case ObCharType:
      case ObVarcharType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType:
      case ObJsonType:
      case ObGeometryType: {
        obj.meta_.set_collation_level(meta_.get_collation_level());
        obj.meta_.set_scale(meta_.get_scale());
        obj.val_len_ = val_len_;
        obj.v_ = v_;
        break;
      }
      default: {
        obj.meta_.set_scale(meta_.get_scale());
        obj.val_len_ = val_len_;
        obj.v_ = v_;
        break;
      }
    }
  }

  OB_INLINE void copy_value_or_obj(ObObj &obj, bool is_copy_all) const {
    if (is_copy_all) {
      obj = *this;
    } else {
      obj.meta_.set_scale(meta_.get_scale());
      obj.val_len_ = val_len_;
      obj.v_ = v_;
    }
  }
  //@{ setters
  OB_INLINE void set_type(const ObObjType &type)
  {
    if (OB_UNLIKELY(ObNullType > type || ObMaxType < type)) {
      COMMON_LOG_RET(ERROR, common::OB_ERR_UNEXPECTED, "invalid type", K(type));
      meta_.set_type(ObUnknownType);
    } else {
      meta_.set_type(type);
    }
  }
  static int64_t get_otimestamp_store_size()
  {
    return static_cast<int64_t>(sizeof(int64_t)
        + sizeof(uint16_t));
  }
  template<typename T>
  void set_obj_value(const T &v);
  void set_collation_level(const ObCollationLevel &cs_level) { meta_.set_collation_level(cs_level); }
  void set_collation_type(const ObCollationType &cs_type) { meta_.set_collation_type(cs_type); }
  void set_default_collation_type() { meta_.set_default_collation_type(); }
  void set_meta_type(const ObObjMeta &type) { meta_ = type; }
  void set_collation(const ObObjMeta &type) { meta_.set_collation(type); }
  void set_scale(ObScale scale) { meta_.set_scale(scale); }

  void set_int(const ObObjType type, const int64_t value);
  void set_tinyint(const int8_t value);
  void set_smallint(const int16_t value);
  void set_mediumint(const int32_t value);
  void set_int32(const int32_t value);
  void set_int(const int64_t value);  // aka bigint

  void set_tinyint_value(const int8_t value);
  void set_smallint_value(const int16_t value);
  void set_mediumint_value(const int32_t value);
  void set_int32_value(const int32_t value);
  void set_int_value(const int64_t value);  // aka bigint

  void set_uint(const ObObjType type, const uint64_t value);
  void set_utinyint(const uint8_t value);
  void set_utinyint_value(const uint8_t value);
  void set_usmallint(const uint16_t value);
  void set_usmallint_value(const uint16_t value);
  void set_umediumint(const uint32_t value);
  void set_umediumint_value(const uint32_t value);
  void set_uint32(const uint32_t value);
  void set_uint32_value(const uint32_t value);
  void set_uint64(const uint64_t value);
  void set_uint64_value(const uint64_t value);

  void set_float(const ObObjType type, const float value);
  void set_float(const float value);
  void set_float_value(const float value);
  void set_ufloat(const float value);
  void set_ufloat_value(const float value);

  void set_double(const ObObjType type, const double value);
  void set_double(const double value);
  void set_double_value(const double value);
  void set_udouble(const double value);
  void set_udouble_value(const double value);

  void set_number(const ObObjType type, const number::ObNumber &num);
  void set_number(const ObObjType type, const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits);
  void set_number(const number::ObNumber &num);
  void set_number_value(const number::ObNumber &num);
  void set_number_value(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits);
  void set_number(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits);

  void set_unumber(const number::ObNumber &num);
  void set_unumber(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits);
  void set_unumber_value(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits);
  void set_datetime(const ObObjType type, const int64_t value);
  void set_datetime(const int64_t value);
  void set_mysql_datetime(const int64_t value);
  void set_mysql_datetime(const ObMySQLDateTime value);
  void set_timestamp(const int64_t value);
  void set_date(const int32_t value);
  void set_mysql_date(const int32_t value);
  void set_mysql_date(const ObMySQLDate value);
  void set_time(const int64_t value);
  void set_year(const uint8_t value);

  void set_datetime_value(const int64_t value);
  void set_timestamp_value(const int64_t value);
  void set_date_value(const int32_t value);
  void set_time_value(const int64_t value);
  void set_year_value(const uint8_t value);

  //only set v_.string_ and length
  void set_common_value(const ObString &value);

  void set_string(const ObObjType type, const char *ptr, const ObString::obstr_size_t size);
  void set_string(const ObObjType type, const ObString &value);
  void set_varchar(const ObString &value);
  void set_varchar(const char *ptr, const ObString::obstr_size_t size);
  void set_varchar_value(const char *ptr, const ObString::obstr_size_t size);
  void set_varchar(const char *cstr);
  void set_char(const ObString &value);
  void set_char_value(const char *ptr, const ObString::obstr_size_t size);
  void set_varbinary(const ObString &value);
  void set_binary(const ObString &value);
  void set_raw_value(const char *ptr, const ObString::obstr_size_t size);
  void set_hex_string(const ObString &value);
  void set_hex_string_value(const ObString &value);
  void set_hex_string_value(const char *ptr, const ObString::obstr_size_t size);
  void set_enum(const uint64_t value);
  void set_enum_value(const uint64_t value);
  void set_set(const uint64_t value);
  void set_set_value(const uint64_t value);
  void set_enum_inner(const ObString &value);
  void set_enum_inner(const char *ptr, const ObString::obstr_size_t size);
  void set_set_inner(const ObString &value);
  void set_set_inner(const char *ptr, const ObString::obstr_size_t size);
  void set_lob_value(const ObObjType type, const ObLobCommon *value, const int32_t length);
  void set_lob_value(const ObObjType type, const char *ptr, const int32_t length);
  void set_json_value(const ObObjType type, const ObLobCommon *value, const int32_t length);
  void set_json_value(const ObObjType type, const char *ptr, const int32_t length);
  void set_geometry_value(const ObObjType type, const ObLobCommon *value, const int32_t length);
  void set_geometry_value(const ObObjType type, const char *ptr, const int32_t length);
  inline void set_inrow() { meta_.set_inrow(); }
  inline void set_outrow() { meta_.set_outrow(); }
  inline void set_has_lob_header() { meta_.set_has_lob_header(); }

  inline void set_subschema_id(uint16_t subschema_id) { meta_.set_subschema_id(subschema_id); }
  void set_otimestamp_value(const ObObjType type, const ObOTimestampData &value);

  inline void set_bool(const bool value);
  inline void set_ext(const int64_t value);
  inline void set_extend(const int64_t value, uint8 extend_type, int32_t size = 0);
  inline void set_unknown(const int64_t value);

  inline void set_bit(const uint64_t value);
  inline void set_bit_value(const uint64_t value);

  inline void set_null();
  inline void set_min_value();
  inline void set_max_value();
  inline void set_nop_value();

  inline void set_lob(const char* ptr, const int32_t size, const ObLobScale &lob_scale);

  void set_val_len(const int32_t val_len);
  inline void set_sql_udt(const char* ptr, int32_t size, uint16_t subschema_id = 0, uint8_t udt_flags = 0);
  inline void set_sql_collection(const char* ptr, int32_t size, uint16_t subschema_id = 0, uint8_t udt_flags = 0);
  inline void set_udt_value(const char* ptr, int32_t size);
  //@}

  //@{ getters
  OB_INLINE ObObjType get_type() const { return meta_.get_type(); }
  OB_INLINE ObObjTypeClass get_type_class() const { return meta_.get_type_class(); }
  OB_INLINE ObCollationLevel get_collation_level() const { return meta_.get_collation_level(); }
  OB_INLINE ObCollationType get_collation_type() const { return meta_.get_collation_type(); }
  OB_INLINE ObScale get_scale() const { return meta_.get_scale(); }
  inline const ObObjMeta& get_meta() const { return meta_; }

  inline int get_tinyint(int8_t &value) const;
  inline int get_smallint(int16_t &value) const;
  inline int get_mediumint(int32_t &value) const;
  inline int get_int32(int32_t &value) const;
  inline int get_int(int64_t &value) const;

  inline int get_utinyint(uint8_t &value) const;
  inline int get_usmallint(uint16_t &value) const;
  inline int get_umediumint(uint32_t &value) const;
  inline int get_uint32(uint32_t &value) const;
  inline int get_uint64(uint64_t &value) const;

  inline int get_float(float &value) const;
  inline int get_double(double &value) const;
  inline int get_ufloat(float &value) const;
  inline int get_udouble(double &value) const;

  inline int get_number(number::ObNumber &num) const;
  inline int get_unumber(number::ObNumber &num) const;
  inline int get_datetime(int64_t &value) const;
  inline int get_timestamp(int64_t &value) const;
  inline int get_date(int32_t &value) const;
  inline int get_time(int64_t &value) const;
  inline int get_year(uint8_t &value) const;

  inline int get_string(ObString &value) const;
  inline int get_varchar(ObString &value) const;
  inline int get_char(ObString &value) const;
  inline int get_varbinary(ObString &value) const;
  inline int get_binary(ObString &value) const;
  inline int get_hex_string(ObString &value) const;
  inline int get_print_string(ObString &value) const;

  inline int get_bool(bool &value) const;
  inline int get_ext(int64_t &value) const;
  inline int get_unknown(int64_t &value) const;
  inline int get_bit(uint64_t &value) const;
  inline int get_enum(uint64_t &value) const;
  inline int get_set(uint64_t &value) const;
  int get_enum_str_val(ObSqlString &str_val, const ObIArray<ObString> &type_infos) const;
  int get_set_str_val(ObSqlString &str_val, const ObIArray<ObString> &type_infos) const;
  inline int32_t get_val_len() const { return val_len_; }
  inline int get_enumset_inner_value(ObEnumSetInnerValue &inner_value) const;

  /// the follow getters do not check type, use them when you already known the type
  OB_INLINE int8_t get_tinyint() const { return static_cast<int8_t>(v_.int64_); }
  OB_INLINE int16_t get_smallint() const { return static_cast<int16_t>(v_.int64_); }
  OB_INLINE int32_t get_mediumint() const { return static_cast<int32_t>(v_.int64_); }
  OB_INLINE int32_t get_int32() const { return static_cast<int32_t>(v_.int64_); }
  OB_INLINE int64_t get_int() const { return static_cast<int64_t>(v_.int64_); }

  OB_INLINE uint8_t get_utinyint() const { return static_cast<uint8_t>(v_.uint64_); }
  OB_INLINE uint16_t get_usmallint() const { return static_cast<uint16_t>(v_.uint64_); }
  OB_INLINE uint32_t get_umediumint() const { return static_cast<uint32_t>(v_.uint64_); }
  OB_INLINE uint32_t get_uint32() const { return static_cast<uint32_t>(v_.uint64_); }
  OB_INLINE uint64_t get_uint64() const { return static_cast<uint64_t>(v_.uint64_); }

  OB_INLINE float get_float() const { return v_.float_; }
  OB_INLINE double get_double() const { return v_.double_; }
  OB_INLINE float get_ufloat() const { return v_.float_; }
  OB_INLINE double get_udouble() const { return v_.double_; }

  OB_INLINE bool is_negative_number() const { return number::ObNumber::is_negative_number(nmb_desc_); }
  OB_INLINE bool is_zero_number() const { return number::ObNumber::is_zero_number(nmb_desc_); }
  OB_INLINE int64_t get_number_digit_length() const { return nmb_desc_.len_; }
  OB_INLINE int64_t get_number_byte_length() const { return nmb_desc_.len_ * sizeof(uint32_t); }
  OB_INLINE number::ObNumber get_number() const { return number::ObNumber(nmb_desc_.desc_, v_.nmb_digits_); }
  OB_INLINE number::ObNumber get_unumber() const { return number::ObNumber(nmb_desc_.desc_, v_.nmb_digits_); }
  OB_INLINE bool is_zero_decimalint() const { return val_len_ == 0; }

  OB_INLINE int64_t get_datetime() const { return v_.datetime_; }
  OB_INLINE ObMySQLDateTime get_mysql_datetime() const { return v_.datetime_; }
  OB_INLINE int64_t get_timestamp() const { return v_.datetime_; }
  OB_INLINE int32_t get_date() const { return v_.date_; }
  OB_INLINE ObMySQLDate get_mysql_date() const { return v_.date_; }
  OB_INLINE int64_t get_time() const { return v_.time_; }
  OB_INLINE uint8_t get_year() const { return v_.year_; }

  OB_INLINE ObString get_string() const {
    return ObString(val_len_, v_.string_);
  }
  OB_INLINE ObString get_varchar() const { return ObString(val_len_, v_.string_); }
  OB_INLINE ObString get_char() const { return ObString(val_len_, v_.string_); }
  OB_INLINE ObString get_varbinary() const { return ObString(val_len_, v_.string_); }
  OB_INLINE ObString get_binary() const { return ObString(val_len_, v_.string_); }
  OB_INLINE ObString get_hex_string() const { return ObString(val_len_, v_.string_); }
  OB_INLINE ObString get_print_string(const int64_t max_len) const { return ObString(MIN(val_len_, max_len), v_.string_); }
  OB_INLINE ObString get_text_print_string(const int64_t max_len) const {
    int ret = OB_SUCCESS;
    ObString inrow_data;
    if (val_len_ == 0 || !has_lob_header()) {
      inrow_data.assign_ptr(v_.string_, val_len_);
    } else {
      ObLobLocatorV2 loc(reinterpret_cast<char *>(v_.ptr_), val_len_, has_lob_header());
      if (OB_UNLIKELY(!loc.is_valid(false))) {
        inrow_data.assign_ptr(v_.string_, val_len_);
      } else if (!loc.has_inrow_data()) {
        inrow_data.assign_ptr("outrow", 6);
      } else if (OB_FAIL(loc.get_inrow_data(inrow_data))) {
      } else {
        inrow_data.assign_ptr(inrow_data.ptr(), MIN(inrow_data.length(), max_len));
      }
    }
    return inrow_data;
  }

  OB_INLINE int get_json_print_data(ObString &json_data, char *buf, int64_t buf_len, int64_t &pos) const {
    int ret = OB_SUCCESS;
    json_data = get_string();
    ObLobCommon* lob_comm = reinterpret_cast<ObLobCommon*>(json_data.ptr());
    bool bret = has_lob_header() ||
                (json_data.length() >= sizeof(ObLobCommon) && lob_comm->is_valid());
    if (!bret) {
      // if it called in log params, like K(json), it maybe a json with disk lob header only
      // cannot judge here is a plain json data or json data with disk lob header
    } else {
      ObLobLocatorV2 loc(reinterpret_cast<char *>(v_.ptr_), val_len_, bret);
      if (OB_UNLIKELY(!loc.is_valid(false))) {
        // do nothing, warn log inside
        COMMON_LOG(WARN, "Lob: invalid json lob", K(ret), K(json_data));
      } else if (!loc.has_inrow_data()) {
        if (OB_FAIL(databuff_printf(buf, buf_len, pos, "%s", "'outrow json'"))) {
        }
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(loc.get_inrow_data(json_data))) {
      }
    }
    return ret;
  }

  OB_INLINE int get_udt_print_data(ObString &udt_data, char *buf, int64_t buf_len, int64_t &pos, bool is_lob) const {
    int ret = OB_SUCCESS;
    udt_data = get_string();
    if (is_lob) {
      ObLobLocatorV2 loc(reinterpret_cast<char *>(v_.ptr_), val_len_, true);
      if (OB_UNLIKELY(!loc.is_valid(false))) {
        // do nothing, warn log inside
        COMMON_LOG(WARN, "Lob: invalid udt lob", K(ret), K(udt_data));
      } else if (!loc.is_inrow()) {
        if (OB_FAIL(databuff_printf(buf, buf_len, pos, "%s", "'outrow udt'"))) {
        }
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(loc.get_inrow_data(udt_data))) {
      }
    }
    return ret;
  }

  OB_INLINE bool get_bool() const { return (0 != v_.int64_); }
  inline int64_t get_ext() const;
  int get_real_param_count(int64_t &count) const;
  OB_INLINE int64_t get_unknown() const { return v_.unknown_; }
  OB_INLINE uint64_t get_bit() const { return v_.uint64_; }
  OB_INLINE uint64_t get_enum() const { return v_.uint64_; }
  OB_INLINE uint64_t get_set() const { return v_.uint64_; }
  OB_INLINE ObString get_set_inner() const { return ObString(val_len_, v_.string_); }
  OB_INLINE ObString get_enum_inner() const { return ObString(val_len_, v_.string_); }

  inline const number::ObNumber::Desc& get_number_desc() const { return nmb_desc_; }
  inline const uint32_t *get_number_digits() const { return v_.nmb_digits_; }

  inline const char* get_string_ptr() const { return v_.string_; }
  inline int32_t get_string_len() const { return val_len_; }
  inline const ObLobCommon *get_lob_value() const { return v_.lob_; }
  inline int get_lob_value(const ObLobCommon *&lob) const;
  inline int get_lob_locatorv2(ObLobLocatorV2 &lob_locator) const;
  inline int get_lob_locatorv2(ObString &lob_locator) const;
  //TODO @shanting dangerous interface

  inline ObOTimestampData::UnionTZCtx get_tz_desc() const { return time_ctx_; }
  inline ObOTimestampData get_otimestamp_value() const { return ObOTimestampData(v_.datetime_, time_ctx_); }

  inline const ObDecimalInt *get_decimal_int() const
  {
    return v_.decimal_int_;
  }
  inline void set_decimal_int(int32_t int_bytes, ObScale scale, ObDecimalInt *decimal_int)
  {
    meta_.set_decimal_int(scale);
    val_len_ = int_bytes;
    v_.decimal_int_ = decimal_int;
  }

  inline int32_t get_int_bytes() const {
    return val_len_;
  }
  inline uint16_t get_udt_subschema_id() const { return meta_.get_subschema_id(); }
  inline uint8_t get_udt_flags() const { return meta_.get_udt_flags(); }

  //@}

  //@{ test functions
  OB_INLINE bool is_valid_type() const { return meta_.is_valid(); }
  OB_INLINE bool is_invalid_type() const { return meta_.is_invalid(); }

  OB_INLINE bool is_null() const { return meta_.is_null(); }
  OB_INLINE bool is_null_or_empty_string() const { return meta_.is_null()
      || (meta_.is_character_type() && (0 == get_string_len())); }
  OB_INLINE bool is_tinyint() const { return meta_.is_tinyint(); }
  OB_INLINE bool is_smallint() const { return meta_.is_smallint(); }
  OB_INLINE bool is_mediumint() const { return meta_.is_mediumint(); }
  OB_INLINE bool is_int32() const { return meta_.is_int32(); }
  OB_INLINE bool is_int() const { return meta_.is_int(); }
  OB_INLINE bool is_utinyint() const { return meta_.is_utinyint(); }
  OB_INLINE bool is_usmallint() const { return meta_.is_usmallint(); }
  OB_INLINE bool is_umediumint() const { return meta_.is_umediumint(); }
  OB_INLINE bool is_uint32() const { return meta_.is_uint32(); }
  OB_INLINE bool is_uint64() const { return meta_.is_uint64(); }
  OB_INLINE bool is_float() const { return meta_.is_float(); }
  OB_INLINE bool is_double() const { return meta_.is_double(); }
  OB_INLINE bool is_ufloat() const { return meta_.is_ufloat(); }
  OB_INLINE bool is_udouble() const { return meta_.is_udouble(); }
  OB_INLINE bool is_number() const { return meta_.is_number(); }
  OB_INLINE bool is_unumber() const { return meta_.is_unumber(); }
  OB_INLINE bool is_decimal_or_float() const { return meta_.is_decimal_or_float(); }
  OB_INLINE bool is_datetime() const { return meta_.is_datetime(); }
  OB_INLINE bool is_mysql_datetime() const { return meta_.is_mysql_datetime(); }
  OB_INLINE bool is_timestamp() const { return meta_.is_timestamp(); }
  OB_INLINE bool is_year() const { return meta_.is_year(); }
  OB_INLINE bool is_date() const { return meta_.is_date(); }
  OB_INLINE bool is_mysql_date() const { return meta_.is_mysql_date(); }
  OB_INLINE bool is_time() const { return meta_.is_time(); }
  OB_INLINE bool is_varchar() const { return meta_.is_varchar(); }
  OB_INLINE bool is_char() const { return meta_.is_char(); }
  OB_INLINE bool is_varbinary() const { return meta_.is_varbinary(); }
  OB_INLINE bool is_binary() const { return meta_.is_binary(); }
  OB_INLINE bool is_hex_string() const { return meta_.is_hex_string(); }
  OB_INLINE bool is_ext() const { return meta_.is_ext(); }
  OB_INLINE bool is_unknown() const { return meta_.is_unknown(); }
  OB_INLINE bool is_bit() const { return meta_.is_bit(); }
  OB_INLINE bool is_enum() const { return meta_.is_enum(); }
  OB_INLINE bool is_set() const { return meta_.is_set(); }
  OB_INLINE bool is_text() const { return meta_.is_text(); }
  OB_INLINE bool is_clob() const { return meta_.is_clob(); }
  OB_INLINE bool is_blob() const { return meta_.is_blob(); }
  OB_INLINE bool is_lob_storage() const { return meta_.is_lob_storage(); }
  OB_INLINE bool is_lob() const { return meta_.is_lob(); }
  OB_INLINE bool is_outrow_lob() const;
  OB_INLINE bool is_delta_tmp_lob() const;
  OB_INLINE bool is_persist_lob() const;
  OB_INLINE bool is_inrow() const { return meta_.is_inrow(); }
  OB_INLINE bool is_outrow() const { return meta_.is_outrow(); }
  OB_INLINE bool has_lob_header() const { return meta_.has_lob_header(); }

  OB_INLINE bool is_json() const { return meta_.is_json(); }
  OB_INLINE bool is_json_inrow() const { return meta_.is_json_inrow(); }
  OB_INLINE bool is_json_outrow() const { return meta_.is_json_outrow(); }

  OB_INLINE bool is_geometry() const { return meta_.is_geometry(); }
  OB_INLINE bool is_geometry_inrow() const { return meta_.is_geometry_inrow(); }
  OB_INLINE bool is_geometry_outrow() const { return meta_.is_geometry_outrow(); }
  OB_INLINE bool is_collection_sql_type() const { return meta_.is_collection_sql_type(); }


  // including int only
  OB_INLINE bool is_unsigned_integer() const { return meta_.is_unsigned_integer(); }
  OB_INLINE bool is_signed_integer() const { return meta_.is_signed_integer(); }
  // including int, number,float,double
  OB_INLINE bool is_unsigned() const { return meta_.is_unsigned(); }
  OB_INLINE bool is_integer_type() const { return meta_.is_integer_type(); }
  OB_INLINE bool is_numeric_type() const { return meta_.is_numeric_type(); }
  OB_INLINE bool is_string_type() const { return meta_.is_string_type(); }
  OB_INLINE bool is_temporal_type() const { return meta_.is_temporal_type(); }
  OB_INLINE bool is_varchar_or_char() const { return meta_.is_varchar_or_char(); }
  OB_INLINE bool is_varbinary_or_binary() const { return meta_.is_varbinary_or_binary(); }
  OB_INLINE bool is_fixed_len_char_type() const { return meta_.is_fixed_len_char_type(); }
  OB_INLINE bool is_varying_len_char_type() const { return meta_.is_varying_len_char_type(); }
  OB_INLINE bool is_character_type() const { return meta_.is_character_type(); }
  OB_INLINE bool is_collation_free_compatible() const { return meta_.is_collation_free_compatible(); }
  OB_INLINE bool is_fixed_double() const { return ob_is_double_type(meta_.get_type()) &&
    SCALE_UNKNOWN_YET < meta_.get_scale() && OB_MAX_DOUBLE_FLOAT_SCALE >= meta_.get_scale(); }
  OB_INLINE bool is_string_or_lob_locator_type() const {
    return meta_.is_string_or_lob_locator_type();
  }
  OB_INLINE bool is_pl_extend() const
  {
    return is_ext()
        && !ObObj::is_ext_val(get_ext())
        && meta_.get_extend_type() > 0
        && meta_.get_extend_type() < T_EXT_SQL_ARRAY;
  }
  OB_INLINE bool is_ext_sql_array() const { return meta_.is_ext_sql_array(); }
  OB_INLINE bool is_overflow_integer(ObObjType type) const
  {
#define CHECK_VAL_OVERFLOW(min, max)                            \
    if (is_signed_integer()) {                                  \
      bret = !(get_int() >= min && get_int() <= max);           \
    } else if (is_unsigned_integer()) {                         \
      bret = !(get_uint64() <= max);                            \
    } else {                                                    \
      bret = true;                                              \
    }
    bool bret = false;
    switch (type) {
    case ObTinyIntType: {
      CHECK_VAL_OVERFLOW(INT8_MIN, INT8_MAX);
      break;
    }
    case ObSmallIntType: {
      CHECK_VAL_OVERFLOW(INT16_MIN, INT16_MAX);
      break;
    }
    case ObMediumIntType: {
      CHECK_VAL_OVERFLOW(-8388608, 8388607);
      break;
    }
    case ObInt32Type: {
      CHECK_VAL_OVERFLOW(INT32_MIN, INT32_MAX);
      break;
    }
    case ObIntType: {
      CHECK_VAL_OVERFLOW(INT64_MIN, INT64_MAX);
      break;
    }
    case ObUTinyIntType: {
      CHECK_VAL_OVERFLOW(0, UINT8_MAX);
      break;
    }
    case ObUSmallIntType: {
      CHECK_VAL_OVERFLOW(0, UINT16_MAX);
      break;
    }
    case ObUMediumIntType: {
      CHECK_VAL_OVERFLOW(0, 16777215);
      break;
    }
    case ObUInt32Type: {
      CHECK_VAL_OVERFLOW(0, UINT32_MAX);
      break;
    }
    case ObUInt64Type: {
      CHECK_VAL_OVERFLOW(0, UINT64_MAX);
      break;
    }
    default: {
      bret = true;
      break;
    }
    }
#undef CHECK_VAL_OVERFLOW
    return bret;
  }

  inline bool is_decimal_int() const { return meta_.is_decimal_int(); }
  inline bool is_min_value() const;
  inline bool is_max_value() const;
  inline bool is_nop_value() const;
  inline bool is_true() const;
  inline bool is_false() const;

  bool is_zero() const;
  //@}

  /// apply mutation to this obj
  int apply(const ObObj &mutation);

  //@{ comparison
  //
  // ATTENTION:
  //
  // When < > <= >= == != compare is_equal is called,
  // that_obj MUST have same type with this obj (*this)
  // or can_compare.
  // For Sql, to get diff type comparing result(varchar '123' compare with int 123),
  // call static function ObExprEqual::calc or other ObExprXX::calc.
  bool operator<(const ObObj &that_obj) const;
  bool operator>(const ObObj &that_obj) const;
  bool operator==(const ObObj &that_obj) const;
  bool operator!=(const ObObj &that_obj) const;
  bool can_compare(const ObObj &other) const;
  inline bool strict_equal(const ObObj &other) const;
  int check_collation_free_and_compare(const ObObj &other, int &cmp) const;
  int check_collation_free_and_compare(const ObObj &other) const;
  int compare(const ObObj &other, int &cmp) const;
  int compare(const ObObj &other) const;
  int compare(const ObObj &other, ObCollationType cs_type, int &cmp) const;
  int compare(const ObObj &other, ObCollationType cs_type) const;
  int compare(const ObObj &other, common::ObCompareCtx &cmp_ctx) const;
  int equal(const ObObj &other, bool &is_equal) const;
  bool is_equal(const ObObj &other, ObCollationType cs_type) const;
  //@}

  //@{ print utilities
  /// print as JSON style
  int64_t to_string(char *buffer, const int64_t length, const ObObjPrintParams &params = ObObjPrintParams()) const;
  /// print as SQL literal style, e.g. used to show column default value
  int print_sql_literal(char *buffer, int64_t length, int64_t &pos, const ObObjPrintParams &params = ObObjPrintParams()) const;
  /// print as SQL literal sytle and the length can be auto extended, e.g. used to show column default value
  int print_sql_literal(char *&buffer, int64_t &length,
                        int64_t &pos, ObIAllocator &alloc,
                        const ObObjPrintParams &params = ObObjPrintParams()) const;
  /// print as SQL VARCHAR literal
  int print_varchar_literal(char *buffer, int64_t length, int64_t &pos, const ObObjPrintParams &params = ObObjPrintParams()) const;
  //used for enum and set
  int print_varchar_literal(const ObIArray<ObString> &type_infos, char *buffer, int64_t length, int64_t &pos) const;
  /// print as plain string
  int print_plain_str_literal(char *buffer, int64_t length, int64_t &pos, const ObObjPrintParams &params = ObObjPrintParams()) const;
  //used for enum and set
  int print_plain_str_literal(const ObIArray<ObString> &type_infos, char *buffer, int64_t length, int64_t &pos) const;

  void print_range_value(char *buffer, int64_t length, int64_t &pos) const;
  void print_str_with_repeat(char *buffer, int64_t length, int64_t &pos) const;

  //print_smart and print_format are for log_tool use
  int print_smart(char *buffer, int64_t length, int64_t &pos) const;
  int print_format(char *buffer, int64_t length, int64_t &pos) const;
  /// dump into log
  //@}

  //@{  deep copy
  bool need_deep_copy()const;
  OB_INLINE int64_t get_deep_copy_size() const;
  int deep_copy(const ObObj &src, char *buf, const int64_t size, int64_t &pos);
  void* get_deep_copy_obj_ptr();
  void set_data_ptr(void *data_ptr);
  const void *get_data_ptr() const;
  //return byte length
  int64_t get_data_length() const;

  const void *get_tight_data_ptr() const;
  int64_t get_tight_data_len() const;

  template <typename Allocator>
  int to_collation_free_obj(ObObj &dst, bool &is_valid_collation_free, Allocator &allocator);
  //@}

  //@{ checksum
  // CRC64
  void checksum(ObBatchChecksum &bc) const;
  // mysql hash for string, murmurhash for others
  int hash(uint64_t &res) const;
  int hash(uint64_t &res, uint64_t seed) const;
  int hash_murmur(uint64_t &res, uint64_t seed = 0) const;
  // wyhash for all types
  // xx hash
  // mysql hash
  uint64_t varchar_hash(ObCollationType cs_type, uint64_t seed = 0) const;
  uint64_t varchar_murmur_hash(ObCollationType cs_type, uint64_t seed = 0) const;
  uint64_t varchar_wy_hash(ObCollationType cs_type, uint64_t seed = 0) const;
  uint64_t varchar_xx_hash(ObCollationType cs_type, uint64_t seed = 0) const;
  bool check_collation_integrity() const;
  //@}

  NEED_SERIALIZE_AND_DESERIALIZE;

  static uint32_t meta_offset_bits() { return offsetof(ObObj, meta_) * 8; }
  static uint32_t val_len_offset_bits() { return offsetof(ObObj, val_len_) * 8; }
  static uint32_t nmb_desc_offset_bits() { return offsetof(ObObj, nmb_desc_) * 8; }
  static uint32_t v_offset_bits() { return offsetof(ObObj, v_) * 8; }
  int get_char_length(const ObAccuracy accuracy, int32_t &char_len) const;
  int convert_string_value_charset(ObCharsetType charset_type, ObIAllocator &allocator);
  int read_lob_data(ObIAllocator &allocator, ObString &data) const;
private:
  friend class tests::common::ObjTest;
  friend class ObCompactCellWriter;
  friend class ObCompactCellIterator;

public:
  ObObjMeta meta_;  // sizeof = 4
  union
  {
    int32_t val_len_;
    int32_t interval_fractional_; //values for intervalds type
    number::ObNumber::Desc nmb_desc_;
    ObOTimestampData::UnionTZCtx time_ctx_;
  };  // sizeof = 4
  ObObjValue v_;  // sizeof = 8
};
// Create a class for each hash function, all containing a static method named hash, with parameter types const void*, uint64_t, uint64_t.
// is_varchar_hash is for compatibility with the original implementation of murmurhash,processing varchar type, where the underlying does not actually use murmurhash
struct ObjHashBase
{
  static const bool is_varchar_hash = true;
};

// default hash method: same with ObObj::hash())
//  murmurhash for non string types.
//  mysql string hash for string types.
struct ObDefaultHash : public ObjHashBase
{
  static const bool is_varchar_hash = false;
  static uint64_t hash(const void *data, uint64_t len, uint64_t seed)
  {
    return murmurhash64A(data, static_cast<int32_t>(len), seed);
  }
};

struct ObMurmurHash : public ObjHashBase
{
  OB_INLINE static uint64_t hash(const void *data, uint64_t len, uint64_t seed)
  {
    return murmurhash64A(data, static_cast<int32_t>(len), seed);
  }
};

struct ObWyHash : public ObjHashBase
{
  static uint64_t hash(const void *data, uint64_t len, uint64_t seed)
  {
    return wyhash(data, len, seed);
  }
};

struct ObXxHash : public ObjHashBase
{
  static uint64_t hash(const void *data, uint64_t len, uint64_t seed)
  {
    return XXH64(data, static_cast<size_t>(len), seed);
  }
};

template <ObObjType type, typename T, typename P>
struct ObjHashCalculator
{
  static int calc_hash_value(const P &param, const uint64_t hash, uint64_t &res) {
    UNUSED(param);
    UNUSED(hash);
    res = 0;
    return OB_SUCCESS;
  }
};

inline ObObj::ObObj()
{
  reset();
}

inline ObObj::ObObj(bool val)
{
  set_bool(val);
  val_len_ = 0;
}

inline ObObj::ObObj(int32_t val)
{
  set_int32(val);
  val_len_ = 0;
}

inline ObObj::ObObj(int64_t val)
{
  set_int(val);
  val_len_ = 0;
}

inline ObObj::ObObj(ObObjType type)
{
  meta_.set_type(type);
  val_len_ = 0;
}

inline ObObj::ObObj(const ObObj &other)
{
  *this = other;
}

inline void ObObj::reset()
{
  meta_.reset();
  val_len_ = 0;
  v_.int64_ = 0;
}

inline ObObj ObObj::make_min_obj()
{
  ObObj obj;
  obj.set_min_value();
  return obj;
}

inline ObObj ObObj::make_max_obj()
{
  ObObj obj;
  obj.set_max_value();
  return obj;
}

inline ObObj ObObj::make_nop_obj()
{
  ObObj obj;
  obj.set_nop_value();
  return obj;
}

inline void ObObj::set_int(const ObObjType type, const int64_t value)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_NUMERIC);
  v_.int64_ = value;
}

inline void ObObj::set_tinyint(const int8_t value)
{
  meta_.set_tinyint();
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_tinyint_value(const int8_t value)
{
//  meta_.set_tinyint();
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_smallint(const int16_t value)
{
  meta_.set_smallint();
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_smallint_value(const int16_t value)
{
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_mediumint(const int32_t value)
{
  meta_.set_mediumint();
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_mediumint_value(const int32_t value)
{
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_int32(const int32_t value)
{
  meta_.set_int32();
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_int32_value(const int32_t value)
{
//  meta_.set_int32();
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_int(const int64_t value)
{
  meta_.set_int();
  v_.int64_ = value;
}

inline void ObObj::set_int_value(const int64_t value)
{
  v_.int64_ = value;
}

inline void ObObj::set_uint(const ObObjType type, const uint64_t value)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_NUMERIC);
  v_.uint64_ = value;
}

inline void ObObj::set_utinyint(const uint8_t value)
{
  meta_.set_utinyint();
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_utinyint_value(const uint8_t value)
{
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_usmallint(const uint16_t value)
{
  meta_.set_usmallint();
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_usmallint_value(const uint16_t value)
{
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_umediumint(const uint32_t value)
{
  meta_.set_umediumint();
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_umediumint_value(const uint32_t value)
{
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_uint32(const uint32_t value)
{
  meta_.set_uint32();
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_uint32_value(const uint32_t value)
{
  v_.uint64_ = static_cast<uint64_t>(value);
}

inline void ObObj::set_uint64(const uint64_t value)
{
  meta_.set_uint64();
  v_.uint64_ = value;
}

inline void ObObj::set_uint64_value(const uint64_t value)
{
  v_.uint64_ = value;
}

inline void ObObj::set_float(const ObObjType type, const float value)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_NUMERIC);
  v_.uint64_ = 0;
  v_.float_ = value;
}

inline void ObObj::set_float(const float value)
{
  meta_.set_float();
  v_.uint64_ = 0;
  v_.float_ = value;
}

inline void ObObj::set_float_value(const float value)
{
//  meta_.set_float();
  v_.uint64_ = 0;
  v_.float_ = value;
}

inline void ObObj::set_ufloat(const float value)
{
  meta_.set_ufloat();
  v_.uint64_ = 0;
  v_.float_ = value;
}

inline void ObObj::set_ufloat_value(const float value)
{
  v_.uint64_ = 0;
  v_.float_ = value;
}

inline void ObObj::set_double(const ObObjType type, const double value)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_NUMERIC);
  v_.double_ = value;
}

inline void ObObj::set_double(const double value)
{
  meta_.set_double();
  v_.double_ = value;
}

inline void ObObj::set_double_value(const double value)
{
  v_.double_ = value;
}

inline void ObObj::set_udouble(const double value)
{
  meta_.set_udouble();
  v_.double_ = value;
}

inline void ObObj::set_udouble_value(const double value)
{
  v_.double_ = value;
}

inline void ObObj::set_number(const ObObjType type, const number::ObNumber &num)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_NUMERIC);
  nmb_desc_.desc_ = num.get_desc_value();
  v_.nmb_digits_ = num.get_digits();
}

inline void ObObj::set_number(const ObObjType type, const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_NUMERIC);
  nmb_desc_ = nmb_desc;
  v_.nmb_digits_ = nmb_digits;
}

inline void ObObj::set_number(const number::ObNumber &num)
{
  meta_.set_number();
  nmb_desc_.desc_ = num.get_desc_value();
  v_.nmb_digits_ = num.get_digits();
}

inline void ObObj::set_number_value(const number::ObNumber &num)
{
  nmb_desc_.desc_ = num.get_desc_value();
  v_.nmb_digits_ = num.get_digits();
}

inline void ObObj::set_number_value(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits)
{
  nmb_desc_ = nmb_desc;
  v_.nmb_digits_ = nmb_digits;
}

inline void ObObj::set_number(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits)
{
  meta_.set_number();
  nmb_desc_ = nmb_desc;
  v_.nmb_digits_ = nmb_digits;
}

inline void ObObj::set_unumber(const number::ObNumber &num)
{
  meta_.set_unumber();
  nmb_desc_.desc_ = num.get_desc_value();
  v_.nmb_digits_ = num.get_digits();
}

inline void ObObj::set_unumber(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits)
{
  meta_.set_unumber();
  nmb_desc_ = nmb_desc;
  v_.nmb_digits_ = nmb_digits;
}

inline void ObObj::set_unumber_value(const number::ObNumber::Desc nmb_desc, uint32_t *nmb_digits)
{
  nmb_desc_ = nmb_desc;
  v_.nmb_digits_ = nmb_digits;
}

inline void ObObj::set_datetime(const ObObjType type, const int64_t value)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_NUMERIC);
  v_.datetime_ = value;
}

inline void ObObj::set_datetime(const int64_t value)
{
  meta_.set_datetime();
  v_.datetime_ = value;
}

inline void ObObj::set_mysql_datetime(const int64_t value)
{
  meta_.set_mysql_datetime();
  v_.datetime_ = value;
}

inline void ObObj::set_mysql_datetime(const ObMySQLDateTime value)
{
  meta_.set_mysql_datetime();
  v_.datetime_ = value.datetime_;
}

inline void ObObj::set_datetime_value(const int64_t value)
{

  v_.datetime_ = value;
}

inline void ObObj::set_timestamp(const int64_t value)
{
  meta_.set_timestamp();
  v_.datetime_ = value;
}

inline void ObObj::set_timestamp_value(const int64_t value)
{
  v_.datetime_ = value;
}

inline void ObObj::set_date(const int32_t value)
{
  meta_.set_date();
  v_.uint64_ = 0;
  v_.date_ = value;
}

inline void ObObj::set_mysql_date(const int32_t value)
{
  meta_.set_mysql_date();
  v_.uint64_ = 0;
  v_.date_ = value;
}

inline void ObObj::set_mysql_date(const ObMySQLDate value)
{
  meta_.set_mysql_date();
  v_.uint64_ = 0;
  v_.date_ = value.date_;
}

inline void ObObj::set_time(const int64_t value)
{
  meta_.set_time();
  v_.time_ = value;
}
inline void ObObj::set_date_value(const int32_t value)
{
  v_.uint64_ = 0;
  v_.date_ = value;
}

inline void ObObj::set_time_value(const int64_t value)
{
  v_.time_ = value;
}

inline void ObObj::set_year(const uint8_t value)
{
  meta_.set_year();
  v_.uint64_ = 0;
  v_.year_ = value;
}

inline void ObObj::set_year_value(const uint8_t value)
{
  v_.uint64_ = 0;
  v_.year_ = value;
}

inline void ObObj::set_common_value(const ObString &value)
{
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_string(const ObObjType type, const char *ptr, const ObString::obstr_size_t size)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_string(const ObObjType type, const ObString &value)
{
  meta_.set_type(type);
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_varchar(const ObString &value)
{
  meta_.set_varchar();
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_varchar(const char *ptr, const ObString::obstr_size_t size)
{
  meta_.set_varchar();
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_varchar_value(const char *ptr, const ObString::obstr_size_t size)
{
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_varchar(const char *cstr)
{
  meta_.set_varchar();
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = cstr;
  val_len_ = static_cast<int32_t>(strlen(cstr));
}

inline void ObObj::set_char(const ObString &value)
{
  meta_.set_char();
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_char_value(const char *ptr, const ObString::obstr_size_t size)
{
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_varbinary(const ObString &value)
{
  meta_.set_varchar();
  meta_.set_collation_type(CS_TYPE_BINARY);
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_binary(const ObString &value)
{
  meta_.set_char();
  meta_.set_collation_type(CS_TYPE_BINARY);
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_raw_value(const char *ptr, const ObString::obstr_size_t size)
{
  meta_.set_collation_type(CS_TYPE_BINARY);
  meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_hex_string(const ObString &value)
{
  meta_.set_hex_string();
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_hex_string_value(const ObString &value)
{
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_hex_string_value(const char *ptr, const ObString::obstr_size_t size)
{
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_enum(const uint64_t value)
{
  meta_.set_enum();
  v_.uint64_ = value;
}

inline void ObObj::set_set(const uint64_t value)
{
  meta_.set_set();
  v_.uint64_ = value;
}

inline void ObObj::set_enum_value(const uint64_t value)
{
  v_.uint64_ = value;
}

inline void ObObj::set_set_value(const uint64_t value)
{
  v_.uint64_ = value;
}

inline void ObObj::set_enum_inner(const ObString &value)
{
  meta_.set_enum_inner();
  //meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_enum_inner(const char *ptr, const ObString::obstr_size_t size)
{
  meta_.set_enum_inner();
  //meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_set_inner(const ObString &value)
{
  meta_.set_set_inner();
  //meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = value.ptr();
  val_len_ = value.length();
}

inline void ObObj::set_set_inner(const char *ptr, const ObString::obstr_size_t size)
{
  meta_.set_set_inner();
  //meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  v_.string_ = ptr;
  val_len_ = size;
}

// used to set lob value without lob header
// Notice: use set_null for null input!
inline void ObObj::set_lob_value(const ObObjType type, const ObLobCommon *value, const int32_t length)
{
  meta_.set_type(type);
  meta_.set_inrow();
  if (type != ObCollectionSQLType) {
    meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  }
  v_.lob_ = value;
  val_len_ = length;
}

inline void ObObj::set_lob_value(const ObObjType type, const char *ptr, const int32_t length)
{
  meta_.set_type(type);
  meta_.set_inrow();
  if (type != ObCollectionSQLType) {
    meta_.set_collation_level(CS_LEVEL_IMPLICIT);
  }
  v_.string_ = ptr;
  val_len_ = length;
}

inline void ObObj::set_json_value(const ObObjType type, const ObLobCommon *value, const int32_t length)
{
  set_lob_value(type, value, length);
  meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
}

inline void ObObj::set_json_value(const ObObjType type, const char *ptr, const int32_t length)
{
  set_lob_value(type, ptr, length);
  meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
}

inline void ObObj::set_geometry_value(const ObObjType type, const ObLobCommon *value, const int32_t length)
{
  set_lob_value(type, value, length);
  meta_.set_collation_type(CS_TYPE_BINARY);
}

inline void ObObj::set_geometry_value(const ObObjType type, const char *ptr, const int32_t length)
{
  set_lob_value(type, ptr, length);
  meta_.set_collation_type(CS_TYPE_BINARY);
}
inline void ObObj::set_otimestamp_value(const ObObjType type, const ObOTimestampData &value)
{
  meta_.set_otimestamp_type(type);
  time_ctx_ = value.time_ctx_;
  v_.datetime_ = value.time_us_;
}

inline void ObObj::set_null()
{
  meta_.set_null();
}

inline void ObObj::set_bool(const bool value)
{
  meta_.set_tinyint();
  v_.int64_ = static_cast<int64_t>(value);
}

inline void ObObj::set_ext(const int64_t value)
{
  meta_.set_ext();
  val_len_ = 0;
  v_.ext_ = value;
}

inline void ObObj::set_extend(const int64_t value, uint8 extend_type, int32_t size)
{
  set_ext(value);
  meta_.set_extend_type(extend_type);
  set_val_len(size);
}

inline void ObObj::set_unknown(const int64_t value)
{
  meta_.set_unknown();
  v_.unknown_ = value;
}

inline void ObObj::set_bit(const uint64_t value)
{
  meta_.set_bit();
  v_.uint64_ = value;
}

inline void ObObj::set_bit_value(const uint64_t value)
{
  v_.uint64_ = value;
}

inline void ObObj::set_min_value()
{
  set_ext(MIN_OBJECT_VALUE);
}

inline void ObObj::set_max_value()
{
  set_ext(MAX_OBJECT_VALUE);
}

inline void ObObj::set_nop_value()
{
  set_ext(ObActionFlag::OP_NOP);
}

inline void ObObj::set_lob(const char* ptr, const int32_t size, const ObLobScale &lob_scale)
{
  meta_.set_scale(lob_scale.get_scale());
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_sql_udt(const char* ptr, int32_t size, uint16_t subschema_id, uint8_t udt_flags)
{
  meta_.set_sql_udt(subschema_id);
  meta_.set_udt_flags(udt_flags);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_sql_collection(const char* ptr, int32_t size, uint16_t subschema_id, uint8_t udt_flags)
{
  meta_.set_collection(subschema_id);
  // meta_.set_udt_flags(udt_flags);
  v_.string_ = ptr;
  val_len_ = size;
}

inline void ObObj::set_udt_value(const char* ptr, int32_t size)
{
  v_.string_ = ptr;
  val_len_ = size;
}

inline bool ObObj::is_min_value() const
{
  return meta_.get_type() == ObExtendType && v_.ext_ == MIN_OBJECT_VALUE;
}

inline bool ObObj::is_max_value() const
{
  return meta_.get_type() == ObExtendType && v_.ext_ == MAX_OBJECT_VALUE;
}

inline bool ObObj::is_nop_value() const
{
  return meta_.get_type() == ObExtendType && v_.ext_ == ObActionFlag::OP_NOP;
}

inline bool ObObj::is_true() const
{
  return ob_is_int_tc(meta_.get_type()) && 0 != v_.int64_;
}

inline bool ObObj::is_false() const
{
  return ob_is_int_tc(meta_.get_type()) && 0 == v_.int64_;
}

inline bool ObObj::need_deep_copy()const
{
  return (((ob_is_string_type(meta_.get_type())
            || ob_is_json(meta_.get_type())
            || ob_is_geometry(meta_.get_type())
            || ob_is_collection_sql_type(meta_.get_type())) && 0 != val_len_ && NULL != get_string_ptr())
            || (ob_is_number_tc(meta_.get_type())
             && 0 != nmb_desc_.len_ && NULL != get_number_digits())
            || (ob_is_decimal_int(meta_.get_type())
             && 0 != val_len_ && NULL != get_decimal_int()));
}

inline int64_t ObObj::get_ext() const
{
  int64_t res = 0;
  if (ObExtendType == meta_.get_type()) {
    res = v_.ext_;
  }
  return res;
}

inline void ObObj::set_val_len(const int32_t val_len)
{
  val_len_ = val_len;
}

////////////////////////////////////////////////////////////////
inline int ObObj::get_tinyint(int8_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_tinyint()) {
    v = static_cast<int8_t>(v_.int64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_smallint(int16_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_smallint()) {
    v = static_cast<int16_t>(v_.int64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_mediumint(int32_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_mediumint()) {
    v = static_cast<int32_t>(v_.int64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_int32(int32_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_int32()) {
    v = static_cast<int32_t>(v_.int64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_int(int64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_int()) {
    v = v_.int64_;
    ret = OB_SUCCESS;
  }
  return ret;
}


inline int ObObj::get_utinyint(uint8_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_utinyint()) {
    v = static_cast<uint8_t>(v_.uint64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_usmallint(uint16_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_usmallint()) {
    v = static_cast<uint16_t>(v_.uint64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_umediumint(uint32_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_umediumint()) {
    v = static_cast<uint32_t>(v_.uint64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_uint32(uint32_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_uint32()) {
    v = static_cast<uint32_t>(v_.uint64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_uint64(uint64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_uint64()) {
    v = v_.uint64_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_float(float &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_float()) {
    v = v_.float_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_double(double &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_double()) {
    v = v_.double_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_ufloat(float &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_ufloat()) {
    v = v_.float_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_udouble(double &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_udouble()) {
    v = v_.double_;
    ret = OB_SUCCESS;
  }
  return ret;
}


inline int ObObj::get_number(number::ObNumber &num) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_number()) {
    num.assign(nmb_desc_.desc_, v_.nmb_digits_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_unumber(number::ObNumber &num) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_unumber()) {
    num.assign(nmb_desc_.desc_, v_.nmb_digits_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_datetime(int64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_datetime()) {
    v = v_.datetime_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_timestamp(int64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_timestamp()) {
    v = v_.datetime_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_date(int32_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_date()) {
    v = v_.date_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_time(int64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_time()) {
    v = v_.time_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_year(uint8_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_year()) {
    v = v_.year_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_string(ObString &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_string_or_lob_locator_type() || is_lob_storage()) {
    if (is_lob_storage()) {
      if (val_len_ == 0) {
        v.assign_ptr(v_.string_, val_len_);
      } else {
        ObLobLocatorV2 loc(reinterpret_cast<char *>(v_.ptr_), val_len_, has_lob_header());
        if (OB_UNLIKELY(!loc.is_valid())) {
          ret = OB_ERR_UNEXPECTED;
          OB_LOG(WARN, "Unexpected invalid lob locator", K(*this), K(loc));
        } else if (OB_FAIL(loc.get_inrow_data(v))) {
        }
      }
    } else {
      v.assign_ptr(v_.string_, val_len_);
    }
    ret = OB_SUCCESS;
  } else if (meta_.is_null()) {
    v.assign_ptr(NULL, 0);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline bool ObObj::is_outrow_lob() const
{
  bool bret = false;
  if (is_lob_storage()) {
    if (val_len_ == 0) {
    } else if (!has_lob_header()) {
    } else {
      ObLobLocatorV2 loc(reinterpret_cast<char *>(v_.ptr_), val_len_, has_lob_header());
      if (loc.is_valid()) {
        bret = !loc.has_inrow_data();
      }
    }
  }
  return bret;
}

inline bool ObObj::is_delta_tmp_lob() const
{
  bool bret = false;
  if (is_lob_storage()) {
    if (val_len_ == 0) {
    } else if (!has_lob_header()) {
    } else {
      ObLobLocatorV2 loc(reinterpret_cast<char *>(v_.ptr_), val_len_, has_lob_header());
      if (loc.is_valid()) {
        bret = loc.is_delta_temp_lob();
      }
    }
  }
  return bret;
}

inline bool ObObj::is_persist_lob() const
{
  bool bret = false;
  if (ob_is_text_tc(meta_.get_type()) || ob_is_json_tc(meta_.get_type())) {
    if (val_len_ == 0) {
    } else if (!has_lob_header()) {
    } else {
      ObLobLocatorV2 loc(reinterpret_cast<char *>(v_.ptr_), val_len_, has_lob_header());
      if (loc.is_valid()) {
        bret = loc.is_persist_lob();
      }
    }
  }
  return bret;
}

inline int ObObj::get_print_string(ObString &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_lob()) {
    OB_LOG(WARN, "Lob: using get_print_string for text types", K(*this), K(lbt()));
  }
  if (meta_.is_string_type()) {
    v.assign_ptr(v_.string_, MIN(val_len_, OB_MAX_VARCHAR_LENGTH));
    ret = OB_SUCCESS;
  } else if (meta_.is_null()) {
    v.assign_ptr(NULL, 0);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_varchar(ObString &v) const
{
  //int ret = OB_OBJ_TYPE_ERROR;
  //if (meta_.is_varchar()) {
  //  v.assign_ptr(v_.string_, val_len_);
  //  ret = OB_SUCCESS;
  //}
  //return ret;
  //
  ////// todo  distinguish varchar and varbinary ;
  return get_string(v);
}

inline int ObObj::get_char(ObString &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_char()) {
    v.assign_ptr(v_.string_, val_len_);
    ret = OB_SUCCESS;
  } else if (meta_.is_null()) {
    v.assign_ptr(NULL, 0);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_varbinary(ObString &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_varbinary()) {
    v.assign_ptr(v_.string_, val_len_);
    ret = OB_SUCCESS;
  } else if (meta_.is_null()) {
    v.assign_ptr(NULL, 0);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_binary(ObString &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_binary()) {
    v.assign_ptr(v_.string_, val_len_);
    ret = OB_SUCCESS;
  } else if (meta_.is_null()) {
    v.assign_ptr(NULL, 0);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_hex_string(ObString &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_hex_string()) {
    v.assign_ptr(v_.string_, val_len_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_lob_value(const ObLobCommon *&value) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (is_lob_storage()) {
    value = v_.lob_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_lob_locatorv2(ObLobLocatorV2 &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (is_lob_storage()) {
    v.assign_ptr(v_.lob_locator_v2_, val_len_, has_lob_header());
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_lob_locatorv2(ObString &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (is_lob_storage()) {
    v.assign_ptr(v_.string_, val_len_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_bool(bool &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_tinyint()) {
    v = (0 != v_.int64_);
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_ext(int64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_ext()) {
    v = v_.ext_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_unknown(int64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_unknown()) {
    v = v_.unknown_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_bit(uint64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_bit()) {
    v = v_.uint64_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_enum(uint64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_enum()) {
    v = v_.uint64_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_set(uint64_t &v) const
{
  int ret = OB_OBJ_TYPE_ERROR;
  if (meta_.is_set()) {
    v = v_.uint64_;
    ret = OB_SUCCESS;
  }
  return ret;
}

inline int ObObj::get_enumset_inner_value(ObEnumSetInnerValue &inner_value) const
{
  int ret = OB_SUCCESS;
  if (!ob_is_enumset_inner_tc(get_type())) {
    ret = OB_OBJ_TYPE_ERROR;
  } else {
    int64_t pos = 0;
    if (OB_FAIL(inner_value.deserialize(v_.string_, val_len_, pos))) {
    }
  }
  return ret;
}
// All public varchar hash methods in Obj, as well as hash method calls on varchar type obj, all invoke this method
OB_INLINE static uint64_t varchar_hash_with_collation(const ObObj &obj,
                                                      const ObCollationType cs_type,
                                                      const uint64_t hash, hash_algo hash_al)
{
  return ObCharset::hash(cs_type, obj.get_string_ptr(), obj.get_string_len(), hash,
           false, hash_al);
}

inline uint64_t ObObj::varchar_hash(ObCollationType cs_type, uint64_t seed) const
{
  check_collation_integrity();
  return varchar_hash_with_collation(*this, cs_type, seed, NULL);
}

inline uint64_t ObObj::varchar_murmur_hash(ObCollationType cs_type, uint64_t seed) const
{
  check_collation_integrity();
  return varchar_hash_with_collation(*this, cs_type, seed, ObMurmurHash::hash);
}

inline uint64_t ObObj::varchar_wy_hash(ObCollationType cs_type, uint64_t seed) const
{
  check_collation_integrity();
  return varchar_hash_with_collation(*this, cs_type, seed, ObWyHash::hash);
}

inline uint64_t ObObj::varchar_xx_hash(ObCollationType cs_type, uint64_t seed) const
{
  check_collation_integrity();
  return varchar_hash_with_collation(*this, cs_type, seed, ObXxHash::hash);
}

inline const void *ObObj::get_data_ptr() const
{
  const void *ret = NULL;
  if (ob_is_string_type(get_type()) || ob_is_json(get_type())
      || ob_is_geometry(get_type()) || ob_is_collection_sql_type(get_type())) {
    ret = const_cast<char *>(v_.string_);
  } else if (ob_is_number_tc(get_type())) {
    ret = const_cast<uint32_t *>(v_.nmb_digits_);
  } else if (ob_is_decimal_int(get_type())) {
    ret = const_cast<ObDecimalInt *>(v_.decimal_int_);
  } else {
    ret = &v_;
  }
  return ret;
};

inline void ObObj::set_data_ptr(void *data_ptr)
{
  if (ob_is_string_type(get_type()) || ob_is_json(get_type())
      || ob_is_geometry(get_type()) || ob_is_collection_sql_type(get_type())) {
    v_.string_ = static_cast<char*>(data_ptr);
  } else if (ob_is_number_tc(get_type())) {
    v_.nmb_digits_ = static_cast<uint32_t*>(data_ptr);
  } else if (ob_is_decimal_int(get_type())) {
    v_.decimal_int_ = static_cast<ObDecimalInt *>(data_ptr);
  } else {
    //@TODO other value pointer
  }
};

template <typename Allocator>
int ObObj::to_collation_free_obj(ObObj &dst, bool &is_valid_collation_free, Allocator &allocator)
{
  int ret = OB_SUCCESS;
  const int32_t len = get_string_len();
  const bool is_copy_all = true;
  // utf8mb4_general_ci needs the variable-length encoding so that a sort key
  // which is a prefix of another key still has the same ordering as the
  // original values.  Its worst case is an alternating sequence of spaces
  // and one-byte characters (four bytes per space run), plus the terminator.
  const bool use_variable_length_sortkey =
      CS_TYPE_UTF8MB4_GENERAL_CI == get_collation_type();
  const int32_t buf_len = use_variable_length_sortkey ? len * 3 + 2 : len * 2;
  char *buf = NULL;
  is_valid_collation_free = true;
  if (!is_character_type()) {
    ret = OB_INVALID_ARGUMENT;
    COMMON_LOG(WARN, "invalid argument, only varchar or char can be transformed to collation free obj",
        K(ret), "obj type", get_type());
  } else { // For string type, currently only consider char or varchar as rowkey scenarios
    if (0 == len || NULL == get_string_ptr()) {
      copy_value_or_obj(dst, is_copy_all);
      dst.set_collation_type(CS_TYPE_COLLATION_FREE); // TODO: Wait until Juren adds the collation that compares spaces, then further modification is needed
    } else {
      if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(buf_len)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        COMMON_LOG(WARN, "fail to allocate memory", K(ret), K(buf_len));
      } else {
        size_t size = 0;
        if (use_variable_length_sortkey) {
          const bool use_memcmp_semantics = false;
          size = ObCharset::sortkey_var_len(get_collation_type(), get_string_ptr(),
              static_cast<int64_t>(len), buf, static_cast<int64_t>(buf_len),
              use_memcmp_semantics, is_valid_collation_free);
        } else {
          size = ObCharset::sortkey(get_collation_type(), get_string_ptr(),
              static_cast<int64_t>(len), buf, static_cast<int64_t>(buf_len),
              is_valid_collation_free);
        }
        copy_value_or_obj(dst, is_copy_all);
        if (is_varchar()) {
          dst.set_varchar_value(buf, static_cast<int32_t>(size));
        } else {
          dst.set_char_value(buf, static_cast<int32_t>(size));
        }
        dst.set_collation_type(CS_TYPE_COLLATION_FREE);
      }
    }
  }
  return ret;
}

//return byte length
inline int64_t ObObj::get_data_length() const
{
  int64_t ret = sizeof(v_);
  if (ob_is_string_type(get_type()) ||
      ob_is_json(get_type()) ||
      ob_is_geometry(get_type()) ||
      ob_is_collection_sql_type(get_type())) {
    ret = val_len_;
  } else if (ob_is_number_tc(get_type())) {
    ret = nmb_desc_.len_ * sizeof(uint32_t);
  }
  return ret;
};

inline int64_t ObObj::get_tight_data_len() const
{
  int64 len = 0;
  const ObObjType obj_type = get_type();
  if (ob_is_var_len_type(obj_type)) {
    len = val_len_;
  } else if (ob_is_number_tc(obj_type)) {
    len = nmb_desc_.len_ * sizeof(uint32_t);
  } else {  // fixed-length types
    switch(obj_type) {
      case ObNullType:
      case ObTinyIntType:
      case ObUTinyIntType:
      case ObYearType:
        len = 1;
        break;
      case ObSmallIntType:
      case ObUSmallIntType:
        len = 2;
        break;
      case ObMediumIntType:
      case ObUMediumIntType:
      case ObInt32Type:
      case ObUInt32Type:
      case ObFloatType:
      case ObUFloatType:
      case ObDateType:
      case ObMySQLDateType:
        len = 4;
        break;
      case ObIntType:
      case ObUInt64Type:
      case ObDoubleType:
      case ObUDoubleType:
      case ObDateTimeType:
      case ObTimestampType:
      case ObTimeType:
      case ObBitType:
      case ObEnumType:  // @todo according to mysql doc, enum only need 2 bytes to store
      case ObSetType:
      case ObMySQLDateTimeType:
        len = 8;
        break;
      default:
        OB_ASSERT(0);
        break;
    }
  }
  return len;
}

inline const void *ObObj::get_tight_data_ptr() const
{
  const void *ret = NULL;
  const ObObjType obj_type = get_type();
  if (ob_is_var_len_type(obj_type)
      || ob_is_number_tc(obj_type)) {
    ret = v_.ptr_;
  } else {  // fixed-length types
    switch(obj_type) {
      case ObNullType:
      case ObTinyIntType:
      case ObUTinyIntType:
      case ObYearType:
      case ObSmallIntType:
      case ObUSmallIntType:
      case ObMediumIntType:
      case ObUMediumIntType:
      case ObInt32Type:
      case ObUInt32Type:
      case ObFloatType:
      case ObUFloatType:
      case ObDateType:
      case ObIntType:
      case ObUInt64Type:
      case ObDoubleType:
      case ObUDoubleType:
      case ObDateTimeType:
      case ObTimestampType:
      case ObTimeType:
      case ObBitType:
      case ObEnumType:  // @todo according to mysql doc, enum only need 2 bytes to store
      case ObSetType:
      case ObMySQLDateType:
      case ObMySQLDateTimeType:
        ret = &v_;
        break;
      default:
        OB_ASSERT(0);
    }
  }
  return ret;
};

template <typename AllocatorT>
    int ob_write_obj(AllocatorT &allocator, const ObObj &src, ObObj &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.need_deep_copy())) {
    int64_t deep_copy_size = src.get_deep_copy_size();
    char *buf = static_cast<char*>(allocator.alloc(deep_copy_size));
    int64_t pos = 0;
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LIB_LOG(WARN, "allocate memory failed", K(ret), K(deep_copy_size));
    } else if (OB_FAIL(dst.deep_copy(src, buf, deep_copy_size, pos))) {
    }
  } else {
    dst = src;
  }
  return ret;
}

inline bool ObObj::strict_equal(const ObObj &other) const
{
  bool bret = true;
  if (OB_UNLIKELY(get_type() != other.get_type())) {
    bret = false;
  } else {
    //here must use CS_TYPE_BINARY to compare, avoid spaces at the end of the string be ignored
    bret = (0 == compare(other, CS_TYPE_BINARY));
  }
  return bret;
}

#define DEFINE_SET_COMMON_OBJ_VALUE(VTYPE, OBJTYPE)             \
template<>                                                      \
inline void ObObj::set_obj_value<OBJTYPE>(const OBJTYPE & v)    \
{                                                               \
  v_.VTYPE##_ = static_cast<__typeof__(v_.VTYPE##_)>(v);            \
}

DEFINE_SET_COMMON_OBJ_VALUE(int64, int8_t);
DEFINE_SET_COMMON_OBJ_VALUE(uint64, uint8_t);
DEFINE_SET_COMMON_OBJ_VALUE(int64, int16_t);
DEFINE_SET_COMMON_OBJ_VALUE(uint64, uint16_t);
DEFINE_SET_COMMON_OBJ_VALUE(int64, int32_t);
DEFINE_SET_COMMON_OBJ_VALUE(uint64, uint32_t);
DEFINE_SET_COMMON_OBJ_VALUE(float, float);
DEFINE_SET_COMMON_OBJ_VALUE(double, double);
DEFINE_SET_COMMON_OBJ_VALUE(int64, int64_t);
DEFINE_SET_COMMON_OBJ_VALUE(uint64, uint64_t);

template<>
inline void ObObj::set_obj_value<ObString>(const ObString &v)
{
  v_.string_ = v.ptr();
  val_len_ = v.length();
}

template<>
inline void ObObj::set_obj_value<ObOTimestampData>(const ObOTimestampData &otimestamp)
{
  time_ctx_ = otimestamp.time_ctx_;
  v_.datetime_ = otimestamp.time_us_;
}

class ObObjParam : public ObObj
{
public:
  ObObjParam() : ObObj(), accuracy_(), res_flags_(0),
                          raw_text_pos_(-1), raw_text_len_(-1)
  {
  }
  ObObjParam(const ObObj &other) : ObObj(other), accuracy_(), res_flags_(0),
                                                 raw_text_pos_(-1), raw_text_len_(-1)
  {
  }
public:
  void reset();
  // accuracy.
  OB_INLINE void set_accuracy(const common::ObAccuracy &accuracy) { accuracy_.set_accuracy(accuracy); }
  OB_INLINE void set_length(common::ObLength length) { accuracy_.set_length(length); }
  OB_INLINE void set_precision(common::ObPrecision precision) { accuracy_.set_precision(precision); }
  OB_INLINE void set_length_semantics(common::ObLengthSemantics length_semantics) { accuracy_.set_length_semantics(length_semantics); }
  OB_INLINE void set_scale(common::ObScale scale) {
    ObObj::set_scale(scale);
    accuracy_.set_scale(scale);
  }
  OB_INLINE void set_udt_id(uint64_t id) {
    accuracy_.set_accuracy(id);
  }
  OB_INLINE const common::ObAccuracy &get_accuracy() const { return accuracy_; }
  OB_INLINE common::ObLength get_length() const { return accuracy_.get_length(); }
  OB_INLINE common::ObPrecision get_precision() const { return accuracy_.get_precision(); }
  OB_INLINE common::ObScale get_scale() const { return accuracy_.get_scale(); }
  OB_INLINE uint64_t get_udt_id() const { return is_ext() ? accuracy_.get_accuracy() : OB_INVALID_INDEX; }
  OB_INLINE void set_result_flag(uint32_t flag) { res_flags_ |= flag; }
  OB_INLINE void unset_result_flag(uint32_t flag) { res_flags_ &= (~flag); }
  OB_INLINE bool has_result_flag(uint32_t flag) const { return res_flags_ & flag; }
  OB_INLINE uint32_t get_result_flag() const { return res_flags_; }

  OB_INLINE const ParamFlag &get_param_flag() const { return flag_; }
  OB_INLINE ParamFlag &get_param_flag() { return flag_; }
  OB_INLINE bool is_batch_parameters() const { return flag_.is_batch_parameter_; }
  OB_INLINE void set_param_flag(const ParamFlag flag) { flag_ = flag; }
  OB_INLINE void set_need_to_check_type(bool flag) { flag_.need_to_check_type_ = flag; }
  OB_INLINE bool need_to_check_type() const { return flag_.need_to_check_type_; }
  OB_INLINE void set_need_to_check_extend_type(bool flag) { flag_.need_to_check_extend_type_ = flag; }
  OB_INLINE bool need_to_check_extend_type() const { return flag_.need_to_check_extend_type_; }

  OB_INLINE void set_need_to_check_bool_value(bool flag) { flag_.need_to_check_bool_value_ = flag; }
  OB_INLINE bool need_to_check_bool_value() const { return flag_.need_to_check_bool_value_; }

  OB_INLINE void set_expected_bool_value(bool b_value) { flag_.expected_bool_value_ = b_value; }
  OB_INLINE bool expected_bool_value() const { return flag_.expected_bool_value_; }

  OB_INLINE void set_is_pl_mock_default_param(bool flag) { flag_.is_pl_mock_default_param_= flag; }
  OB_INLINE bool is_pl_mock_default_param() const { return flag_.is_pl_mock_default_param_; }
  OB_INLINE void set_is_boolean(bool flag) { flag_.is_boolean_ = flag; }
  OB_INLINE bool is_boolean() const { return flag_.is_boolean_; }

  OB_INLINE void set_ignore_scale_check(bool flag) { flag_.ignore_scale_check_ = flag; }
  OB_INLINE bool ignore_scale_check() const { return flag_.ignore_scale_check_; }

  OB_INLINE void set_raw_text_info(int32_t pos, int32_t len)
  {
    raw_text_pos_ = pos;
    raw_text_len_ = len;
  }
  OB_INLINE int32_t get_raw_text_pos() const { return raw_text_pos_; }
  OB_INLINE int32_t get_raw_text_len() const { return raw_text_len_; }
  OB_INLINE void set_param_meta()
  {
    if (param_meta_.is_null() || ObNullType != get_meta().get_type()) {
      param_meta_ = get_meta();
    }
    if (ob_is_numeric_type(get_type())) {
      ObPrecision default_prec =
        ObAccuracy::DDL_DEFAULT_ACCURACY[get_type()].get_precision();
      ObScale default_scale =
        ObAccuracy::DDL_DEFAULT_ACCURACY[get_type()].get_scale();
      if (get_scale() < 0) {
        if (meta_.get_scale() >= 0) {
          set_scale(meta_.get_scale());
        } else if (ob_is_integer_type(get_type())) {
          set_scale(default_scale);
        }
      }
      if (get_precision() == PRECISION_UNKNOWN_YET) { set_precision(default_prec); }
    }
  }
  OB_INLINE void set_param_meta(const ObObjMeta &meta)
  {
    if (param_meta_.is_null() || ObNullType != meta.get_type()) {
      param_meta_ = meta;
    }
  }
  OB_INLINE const ObObjMeta &get_param_meta() const
  {
    return param_meta_;
  }
  // others.
  INHERIT_TO_STRING_KV(N_OBJ, ObObj, N_ACCURACY, accuracy_,
                       N_FLAG, res_flags_, K_(raw_text_pos), K_(raw_text_len), K_(param_meta));
  NEED_SERIALIZE_AND_DESERIALIZE;

  static uint32_t accuracy_offset_bits() { return offsetof(ObObjParam, accuracy_) * 8; }
  static uint32_t res_flags_offset_bits() { return offsetof(ObObjParam, res_flags_) * 8; }
  static uint32_t flag_offset_bits() { return offsetof(ObObjParam, flag_) * 8; }

private:
  ObAccuracy accuracy_;
  uint32_t res_flags_;  // BINARY, NUM, NOT_NULL, TIMESTAMP, etc
                        // reference: src/lib/regex/include/mysql_com.h
  ParamFlag flag_;
  int32_t raw_text_pos_;
  int32_t raw_text_len_;
  ObObjMeta param_meta_; // meta for objparam, preserves NULL and empty-string metadata
};

struct ObSqlArrayObj
{
  ObSqlArrayObj()
    : data_(nullptr),
      count_(0),
      element_()
  {
  }
  typedef common::ObArrayWrap<common::ObObjParam> DataArray;
  static ObSqlArrayObj *alloc(common::ObIAllocator &allocator, int64_t count);
  static int do_real_deserialize(common::ObIAllocator &allocator, char *buf, int64_t data_len, ObSqlArrayObj *&array_obj);
  int serialize(char* buf, const int64_t buf_len, int64_t& pos) const;
  int deserialize(common::ObIAllocator &allocator, const char* buf, const int64_t data_len, int64_t& pos);
  int64_t get_serialize_size(void) const;
  TO_STRING_KV("data", DataArray(data_, count_), K_(count), K_(element));
  common::ObObjParam *data_;
  int64_t count_;
  common::ObDataType element_;
};

OB_INLINE ObSqlArrayObj *ObSqlArrayObj::alloc(common::ObIAllocator &allocator, int64_t count)
{
  ObSqlArrayObj *array_obj = nullptr;
  void *array_buf = nullptr;
  void *data_buf = nullptr;
  int64_t array_size = sizeof(ObSqlArrayObj) + sizeof(common::ObObjParam) * count;
  if (OB_NOT_NULL(array_buf = allocator.alloc(array_size))) {
    array_obj = new (array_buf) ObSqlArrayObj();
    data_buf = static_cast<char*>(array_buf) + sizeof(ObSqlArrayObj);
    array_obj->data_ = new (data_buf) common::ObObjParam[count];
    array_obj->count_ = count;
  }
  return array_obj;
}

OB_INLINE int64_t ObObj::get_deep_copy_size() const
{
  int64_t ret = 0;
  if (is_string_type() || is_json()
      || is_geometry() || ob_is_decimal_int(get_type()) || is_collection_sql_type()) {
    ret += val_len_;
  } else if (ob_is_number_tc(get_type())) {
    ret += (sizeof(uint32_t) * nmb_desc_.len_);
  }
  return ret;
}

typedef int (*ob_obj_print)(const ObObj &obj, char *buffer, int64_t length, int64_t &pos, const ObObjPrintParams &params);
typedef int64_t (*ob_obj_crc64)(const ObObj &obj, const int64_t current);
typedef void (*ob_obj_batch_checksum)(const ObObj &obj, ObBatchChecksum &bc);
typedef int (*ob_obj_hash)(const ObObj &obj, const uint64_t hash, uint64_t& res);
typedef int (*ob_obj_value_serialize)(const ObObj &obj, char* buf, const int64_t buf_len, int64_t& pos);
typedef int (*ob_obj_value_deserialize)(ObObj &obj, const char* buf, const int64_t data_len, int64_t& pos);
typedef int64_t (*ob_obj_value_get_serialize_size)(const ObObj &obj);
typedef uint64_t (*ob_obj_crc64_v3)(const ObObj &obj, const uint64_t hash);

class ObObjUtil
{
public:
};

class ObHexEscapeSqlStr
{
public:
  ObHexEscapeSqlStr(const common::ObString &str) : str_(str),
                                                   skip_escape_(false),
                                                   do_single_quote_escape_(false) { }
  ObHexEscapeSqlStr(const common::ObString &str, const bool skip_escape) : str_(str),
                                                                           skip_escape_(skip_escape),
                                                                           do_single_quote_escape_(false){ }
  ObHexEscapeSqlStr(const common::ObString &str,
                    const bool skip_escape,
                    const bool do_single_quote_escape)
    : str_(str), skip_escape_(skip_escape), do_single_quote_escape_(do_single_quote_escape){ }
  ObString str() const { return str_; }
  int64_t get_extra_length() const;
  DECLARE_TO_STRING;
private:
  ObString str_;
  bool skip_escape_;
  bool do_single_quote_escape_;
};

typedef Ob2DArray<ObObjParam, OB_MALLOC_BIG_BLOCK_SIZE,
                          ObWrapperAllocator,false,
                          ObSEArray<ObObjParam *, 1, ObWrapperAllocator, false>
                          > ParamStore;

class ObObjUDTUtil
{
// Utils for convert pl udt baisc attributes(obobjs) to sql udt format
// basically, obj meta is store in subschema map
// only value needs serialize into a continuous sql udt buffer
public:
  static int ob_udt_obj_value_serialize(const ObObj &obj, char* buf, const int64_t buf_len, int64_t& pos);
  static int ob_udt_obj_value_deserialize(ObObj &obj, const char* buf, const int64_t data_len, int64_t& pos);
  static int ob_udt_obj_value_get_serialize_size(const ObObj &obj, int64_t &value_len);

  static bool ob_is_supported_sql_udt(const uint64_t udt_id)
  { // only GIS-related SQL UDTs are supported currently
    return udt_id == T_OBJ_SDO_POINT
          || udt_id == T_OBJ_SDO_GEOMETRY
          || udt_id == T_OBJ_SDO_ELEMINFO_ARRAY
          || udt_id == T_OBJ_SDO_ORDINATE_ARRAY;
  }
};

class ObObjCharacterUtil
{
  // Utils for safe hex representation of character types.
  // Only use for the character types that supported as primary key columns.
public:
  static int print_safe_hex_represent_mysql(const ObObj &obj, char *buffer, int64_t length, int64_t &pos);
  static int print_safe_hex_represent(const ObObj &obj, char *buffer, int64_t length, int64_t &pos, const ObAccuracy &accuracy);
};

}
}

#endif //
