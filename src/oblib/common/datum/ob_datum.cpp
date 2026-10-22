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

#define USING_LOG_PREFIX SHARE

#include "ob_datum.h"

namespace oceanbase
{
namespace common
{

ObObjDatumMapType ObDatum::get_obj_datum_map_type(const ObObjType type)
{

  const static ObObjDatumMapType maps[] = {
    OBJ_DATUM_NULL,           // ObNullType
    OBJ_DATUM_8BYTE_DATA,     // ObTinyIntType
    OBJ_DATUM_8BYTE_DATA,     // ObSmallIntType
    OBJ_DATUM_8BYTE_DATA,     // ObMediumIntType
    OBJ_DATUM_8BYTE_DATA,     // ObInt32Type
    OBJ_DATUM_8BYTE_DATA,     // ObIntType
    OBJ_DATUM_8BYTE_DATA,     // ObUTinyIntType
    OBJ_DATUM_8BYTE_DATA,     // ObUSmallIntType
    OBJ_DATUM_8BYTE_DATA,     // ObUMediumIntType
    OBJ_DATUM_8BYTE_DATA,     // ObUInt32Type
    OBJ_DATUM_8BYTE_DATA,     // ObUInt64Type
    OBJ_DATUM_4BYTE_DATA,     // ObFloatType
    OBJ_DATUM_8BYTE_DATA,     // ObDoubleType
    OBJ_DATUM_4BYTE_DATA,     // ObUFloatType
    OBJ_DATUM_8BYTE_DATA,     // ObUDoubleType
    OBJ_DATUM_NUMBER,         // ObNumberType
    OBJ_DATUM_NUMBER,         // ObUNumberType
    OBJ_DATUM_8BYTE_DATA,     // ObDateTimeType
    OBJ_DATUM_8BYTE_DATA,     // ObTimestampType
    OBJ_DATUM_4BYTE_DATA,     // ObDateType
    OBJ_DATUM_8BYTE_DATA,     // ObTimeType
    OBJ_DATUM_1BYTE_DATA,     // ObYearType
    OBJ_DATUM_STRING,         // ObVarcharType
    OBJ_DATUM_STRING,         // ObCharType
    OBJ_DATUM_STRING,         // ObHexStringType
    OBJ_DATUM_EXTEND,           // ObExtendType
    OBJ_DATUM_8BYTE_DATA,     // ObUnknownType
    OBJ_DATUM_STRING,         // ObTinyTextType
    OBJ_DATUM_STRING,         // ObTextType
    OBJ_DATUM_STRING,         // ObMediumTextType
    OBJ_DATUM_STRING,         // ObLongTextType
    OBJ_DATUM_8BYTE_DATA,     // ObBitType
    OBJ_DATUM_8BYTE_DATA,     // ObEnumType
    OBJ_DATUM_8BYTE_DATA,     // ObSetType
    OBJ_DATUM_STRING,         // ObEnumInnerType
    OBJ_DATUM_STRING,         // ObSetInnerType
    OBJ_DATUM_STRING,         // ObJsonType
    OBJ_DATUM_STRING,         // ObGeometryType
    OBJ_DATUM_STRING,         // ObUserDefinedSQLType
    OBJ_DATUM_DECIMALINT,     // ObDecimalIntType
    OBJ_DATUM_STRING,         // ObCollectionSQLType
    OBJ_DATUM_4BYTE_DATA,     // ObMySQLDateType
    OBJ_DATUM_8BYTE_DATA,     // ObMySQLDateTimeType
  };
  static_assert(sizeof(maps) / sizeof(maps[0]) == ObMaxType,
      "new added type should extend this map");
  ObObjDatumMapType t = OBJ_DATUM_MAPPING_MAX;
  if (type < 0 || type >= ObMaxType) {
    LOG_ERROR_RET(common::OB_INVALID_ARGUMENT, "invalid obj type", K(type));
  } else {
    t = maps[type];
  }

  return t;
}

uint32_t ObDatum::get_reserved_size(const ObObjDatumMapType type,
                                    const int16_t prec /*PRECISION_UNKNOWN_YET*/)
{
  static const uint32_t OBOBJ_DATUM_MAP_TYPE_TO_RES_SIZE_MAP[] =
  {
    OBJ_DATUM_NULL_RES_SIZE,           // OBJ_DATUM_NULL
    OBJ_DATUM_STRING_RES_SIZE,         // OBJ_DATUM_STRING
    OBJ_DATUM_NUMBER_RES_SIZE,         // OBJ_DATUM_NUMBER
    OBJ_DATUM_8BYTE_DATA_RES_SIZE,     // OBJ_DATUM_8BYTE_DATA
    OBJ_DATUM_4BYTE_DATA_RES_SIZE,     // OBJ_DATUM_4BYTE_DATA
    OBJ_DATUM_1BYTE_DATA_RES_SIZE,     // OBJ_DATUM_1BYTE_DATA
    OBJ_DATUM_4BYTE_LEN_DATA_RES_SIZE, // OBJ_DATUM_4BYTE_LEN_DATA
    OBJ_DATUM_2BYTE_LEN_DATA_RES_SIZE, // OBJ_DATUM_2BYTE_LEN_DATA
    OBJ_DATUM_EXTEND_RES_SIZE,      // OBJ_DATUM_EXTEND
    OBJ_DATUM_DECIMALINT_MAX_RES_SIZE, // OBJ_DATUM_DECIMALINT
  };
  static_assert(sizeof(OBOBJ_DATUM_MAP_TYPE_TO_RES_SIZE_MAP)
                / sizeof(OBOBJ_DATUM_MAP_TYPE_TO_RES_SIZE_MAP[0]) == OBJ_DATUM_MAPPING_MAX,
      "new added ObObjDatumMapType should extend this map");
  static const uint32_t OBOBJ_DECIMAL_INT_TO_RES_SIZE_MAP[] =
  {
    sizeof(int32_t),   // DECIMAL_INT_32
    sizeof(int64_t),   // DECIMAL_INT_64
    sizeof(int128_t),  // DECIMAL_INT_128
    sizeof(int256_t),  // DECIMAL_INT_256
    sizeof(int512_t),  // DECIMAL_INT_512
  };
  uint32_t res_size = 0;
  if (type >= OBJ_DATUM_MAPPING_MAX) {
    LOG_ERROR_RET(common::OB_INVALID_ARGUMENT, "invalid obj type", K(type));
  } else if (OBJ_DATUM_DECIMALINT == type && prec > 0) {
    const ObDecimalIntWideType decimalint_type = get_decimalint_type(prec);
    res_size = OBOBJ_DECIMAL_INT_TO_RES_SIZE_MAP[decimalint_type];
  } else {
    res_size = OBOBJ_DATUM_MAP_TYPE_TO_RES_SIZE_MAP[type];
  }

  return res_size;
}

DEF_TO_STRING(ObDatum)
{
  int64_t pos = 0;
  int ret = OB_SUCCESS;
  const static int64_t STR_MAX_PRINT_LEN = 128L;
  if (null_) {
    J_NULL();
  } else {
    J_OBJ_START();
    BUF_PRINTF("len: %d, flag: %d, null: %d, ptr: %p", len_, flag_, null_, ptr_);
    if (len_ > 0) {
      OB_ASSERT(NULL != ptr_);
      const int64_t plen = std::min(static_cast<int64_t>(len_),
                                    static_cast<int64_t>(STR_MAX_PRINT_LEN));
      // print hex value
      BUF_PRINTF(", hex: ");
      if (OB_FAIL(hex_print(ptr_, plen, buf, buf_len, pos))) {
        // no logging in to_string function.
      } else {
        // maybe ObIntTC
        if (sizeof(int64_t) == len_) {
          BUF_PRINTF(", int: %ld", *int_);
          // maybe number with one digit
          if (1 == num_->desc_.len_) {
            BUF_PRINTF(", num_digit0: %u", num_->digits_[0]);
          }
        }
        // maybe printable C string
        int64_t idx = 0;
        while (idx < plen && isprint(ptr_[idx])) {
          idx++;
        }
        if (idx >= plen) {
          BUF_PRINTF(", cstr: %.*s", static_cast<int>(plen), ptr_);
        }
      }
    }
    J_OBJ_END();
  }
  return pos;
}

OB_DEF_SERIALIZE(ObDatum)
{
  int ret = OB_SUCCESS;
  const uint32_t wire_pack = get_wire_pack();
  const uint32_t wire_len = is_native_extend() ? DATUM_LEGACY_EXTEND_SIZE : len_;
  OB_UNIS_ENCODE(wire_pack);
  if (OB_SUCC(ret) && wire_len > 0) {
    if (OB_ISNULL(ptr_)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_UNLIKELY(pos > buf_len || wire_len > buf_len - pos)) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      char scratch[DATUM_LEGACY_EXTEND_SIZE];
      const char *payload = is_native_extend() ? get_wire_ext_payload(scratch) : ptr_;
      MEMCPY(buf + pos, payload, wire_len);
      pos += wire_len;
    }
  }
  return ret;
}

OB_DEF_DESERIALIZE(ObDatum)
{
  int ret = OB_SUCCESS;
  uint32_t pack = 0;
  OB_UNIS_DECODE(pack);
  if (OB_SUCC(ret)) {
    ObDatumDesc value;
    value.pack_ = pack;
    if (OB_UNLIKELY(pos > data_len || value.len_ > data_len - pos
        || (value.is_ext() && value.len_ != DATUM_EXTEND_PAYLOAD_SIZE
                           && value.len_ != DATUM_LEGACY_EXTEND_SIZE))) {
      ret = OB_DESERIALIZE_ERROR;
    } else {
      pack_ = pack;
      ptr_ = buf + pos;
      pos += len_;
    }
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObDatum)
{
  int64_t len = 0;
  const uint32_t wire_pack = get_wire_pack();
  OB_UNIS_ADD_LEN(wire_pack);
  len += is_native_extend() ? DATUM_LEGACY_EXTEND_SIZE : len_;
  return len;
}


} // end namespace common
} // end namespace oceanbase
