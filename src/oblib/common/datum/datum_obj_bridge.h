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
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace common
{

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_NULL>(const ObObj &)
{
  set_null();
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_NULL>(ObObj &obj) const
{
  obj.set_null();
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_STRING>(const ObObj &obj)
{
  ptr_ = const_cast<char *>(obj.v_.string_);
  pack_ = obj.val_len_;
  if (obj.is_outrow()) {
    set_outrow();
  }
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_STRING>(ObObj &obj) const
{
  obj.val_len_ = len_;
  obj.v_.string_ = ptr_;
  if (is_outrow()) {
    obj.set_outrow();
  }
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_NUMBER>(const ObObj &obj)
{
  memcpy(no_cv(ptr_), &obj.nmb_desc_, sizeof(obj.nmb_desc_));
  if (OB_LIKELY(1 == obj.nmb_desc_.len_)) {
    memcpy(no_cv(ptr_) + sizeof(obj.nmb_desc_), obj.v_.nmb_digits_, sizeof(*obj.v_.nmb_digits_));
    pack_ = sizeof(obj.nmb_desc_) + sizeof(*obj.v_.nmb_digits_);
  } else {
    memcpy(no_cv(ptr_) + sizeof(obj.nmb_desc_),
        obj.v_.nmb_digits_,
        sizeof(*obj.v_.nmb_digits_) * obj.nmb_desc_.len_);
    pack_ = sizeof(obj.nmb_desc_) + sizeof(*obj.v_.nmb_digits_) * obj.nmb_desc_.len_;
  }
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_NUMBER>(ObObj &obj) const
{
  memcpy(&obj.nmb_desc_, ptr_, sizeof(obj.nmb_desc_));
  obj.v_.string_ = ptr_ + sizeof(obj.nmb_desc_);
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_8BYTE_DATA>(const ObObj &obj)
{
  memcpy(no_cv(ptr_), &obj.v_.uint64_, sizeof(uint64_t));
  pack_ = sizeof(uint64_t);
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_8BYTE_DATA>(ObObj &obj) const
{
  // keep obj.val_len_ is unchanged
  memcpy(&obj.v_.uint64_, ptr_, sizeof(uint64_t));
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_4BYTE_DATA>(const ObObj &obj)
{
  memcpy(no_cv(ptr_), &obj.v_.uint64_, sizeof(uint32_t));
  pack_ = sizeof(uint32_t);
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_4BYTE_DATA>(ObObj &obj) const
{
  // reset obj.v_ first, see ObObj::set_float_value
  obj.v_.uint64_ = 0;
  memcpy(&obj.v_.uint64_, ptr_, sizeof(uint32_t));
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_1BYTE_DATA>(const ObObj &obj)
{
  memcpy(no_cv(ptr_), &obj.v_.uint64_, sizeof(uint8_t));
  pack_ = sizeof(uint8_t);
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_1BYTE_DATA>(ObObj &obj) const
{
  obj.v_.uint64_ = 0;
  memcpy(&obj.v_.uint64_, ptr_, sizeof(uint8_t));
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_4BYTE_LEN_DATA>(const ObObj &obj)
{
  memcpy(no_cv(ptr_), &obj.val_len_, sizeof(uint32_t));
  memcpy(no_cv(ptr_) + sizeof(uint32_t), &obj.v_.uint64_, sizeof(uint64_t));
  pack_ = sizeof(uint32_t) + sizeof(uint64_t);
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_4BYTE_LEN_DATA>(ObObj &obj) const
{
  memcpy(&obj.val_len_, ptr_, sizeof(uint32_t));
  memcpy(&obj.v_.uint64_, ptr_ + sizeof(uint32_t), sizeof(uint64_t));
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_2BYTE_LEN_DATA>(const ObObj &obj)
{
  memcpy(no_cv(ptr_), &obj.val_len_, sizeof(uint16_t));
  memcpy(no_cv(ptr_) + sizeof(uint16_t), &obj.v_.uint64_, sizeof(uint64_t));
  pack_ = sizeof(uint16_t) + sizeof(uint64_t);
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_2BYTE_LEN_DATA>(ObObj &obj) const
{
  // rest obj.val_len_ first, see ObObj::set_otimestamp_value
  obj.val_len_ = 0;
  memcpy(&obj.val_len_, ptr_, sizeof(uint16_t));
  memcpy(&obj.v_.uint64_, ptr_ + sizeof(uint16_t), sizeof(uint64_t));
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_EXTEND>(const ObObj &obj)
{
  OB_ASSERT(obj.is_ext());
  set_extend(obj.get_ext(), obj.get_meta().get_extend_type(), obj.get_val_len(),
              obj.get_meta().get_cs_level(),
              obj.get_meta().get_cs_type());
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_EXTEND>(ObObj &obj) const
{
  if (is_native_extend()) {
    obj.set_extend(get_ext(), get_extend_type(), get_extend_size());
    obj.meta_.set_cs_level(get_extend_collation_level());
    obj.meta_.set_cs_type(get_extend_collation());
  } else {
    // Temporary bridge for remaining legacy callers, not a payload object.
    // The byte codec above and native readers do not depend on this bridge.
    OB_ASSERT(len_ == DATUM_LEGACY_EXTEND_SIZE);
    MEMCPY(&obj.meta_, ptr_, 4);
    MEMCPY(&obj.val_len_, ptr_ + 4, 4);
    MEMCPY(&obj.v_.uint64_, ptr_ + 8, 8);
  }
}

template <>
inline void ObDatum::obj2datum<OBJ_DATUM_DECIMALINT>(const ObObj &obj)
{
  memcpy(no_cv(ptr_), obj.get_decimal_int(), obj.val_len_);
  pack_ = obj.val_len_;
}

template <>
inline void ObDatum::datum2obj<OBJ_DATUM_DECIMALINT>(ObObj &obj) const
{
  obj.v_.decimal_int_ = const_cast<ObDecimalInt *>(decimal_int_);
  obj.val_len_ = len_;
}

inline int ObDatum::from_obj(const ObObj &obj, const ObObjDatumMapType map_type)
{
  int ret = common::OB_SUCCESS;
  if (obj.is_null()) {
    set_null();
  } else {
    switch (map_type) {
      case OBJ_DATUM_NULL: { obj2datum<OBJ_DATUM_NULL>(obj); break; }
      case OBJ_DATUM_STRING: { obj2datum<OBJ_DATUM_STRING>(obj); break; }
      case OBJ_DATUM_NUMBER: { obj2datum<OBJ_DATUM_NUMBER>(obj); break; }
      case OBJ_DATUM_8BYTE_DATA: { obj2datum<OBJ_DATUM_8BYTE_DATA>(obj); break; }
      case OBJ_DATUM_4BYTE_DATA: { obj2datum<OBJ_DATUM_4BYTE_DATA>(obj); break; }
      case OBJ_DATUM_1BYTE_DATA: { obj2datum<OBJ_DATUM_1BYTE_DATA>(obj); break; }
      case OBJ_DATUM_4BYTE_LEN_DATA: { obj2datum<OBJ_DATUM_4BYTE_LEN_DATA>(obj); break; }
      case OBJ_DATUM_2BYTE_LEN_DATA: { obj2datum<OBJ_DATUM_2BYTE_LEN_DATA>(obj); break; }
      case OBJ_DATUM_EXTEND: {
        if (OB_UNLIKELY(!obj.is_ext())) {
          ret = common::OB_ERR_UNEXPECTED;
        } else {
          obj2datum<OBJ_DATUM_EXTEND>(obj);
        }
        break;
      }
      case OBJ_DATUM_DECIMALINT: { obj2datum<OBJ_DATUM_DECIMALINT>(obj); break; }
      case OBJ_DATUM_MAPPING_MAX: {
        ret = common::OB_ERR_UNEXPECTED;
        COMMON_LOG(WARN, "invalid obj datum mapping", K(ret), K(obj), K(map_type));
      }
    }
  }
  return ret;
}

inline int ObDatum::from_obj(const ObObj &obj)
{
  int ret = common::OB_SUCCESS;
  if (obj.is_null()) {
    set_null();
  } else {
    switch (obj.get_type()) {
      case ObNullType: {
        obj2datum<OBJ_DATUM_NULL>(obj);
        break;
      }
      case ObVarcharType:
      case ObCharType:
      case ObHexStringType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType:
      case ObEnumInnerType:
      case ObSetInnerType:
      case ObJsonType:
      case ObGeometryType:
      case ObUserDefinedSQLType:
      case ObCollectionSQLType: {
        obj2datum<OBJ_DATUM_STRING>(obj);
        break;
      }
      case ObNumberType:
      case ObUNumberType: {
        obj2datum<OBJ_DATUM_NUMBER>(obj);
        break;
      }
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType:
      case ObUTinyIntType:
      case ObUSmallIntType:
      case ObUMediumIntType:
      case ObUInt32Type:
      case ObUInt64Type:
      case ObDoubleType:
      case ObUDoubleType:
      case ObDateTimeType:
      case ObTimestampType:
      case ObTimeType:
      case ObUnknownType:
      case ObBitType:
      case ObEnumType:
      case ObSetType:
      case ObMySQLDateTimeType: {
        obj2datum<OBJ_DATUM_8BYTE_DATA>(obj);
        break;
      }
      case ObFloatType:
      case ObUFloatType:
      case ObDateType:
      case ObMySQLDateType: {
        obj2datum<OBJ_DATUM_4BYTE_DATA>(obj);
        break;
      }
      case ObYearType: {
        obj2datum<OBJ_DATUM_1BYTE_DATA>(obj);
        break;
      }
      case ObExtendType: {
        obj2datum<OBJ_DATUM_EXTEND>(obj);
        break;
      }
      case ObDecimalIntType: {
        obj2datum<OBJ_DATUM_DECIMALINT>(obj);
        break;
      }
      case ObMaxType: 
      default: {
        ret = common::OB_ERR_UNEXPECTED;
        COMMON_LOG(WARN, "invalid obj type", K(ret), K(obj), K(obj.get_type()));
      }
    }
  }
  return ret;

}

inline int ObDatum::to_obj(
    ObObj &obj, const ObObjMeta &meta, const ObObjDatumMapType map_type) const
{
  int ret = common::OB_SUCCESS;
  if (is_null()) {
    obj.set_null();
  } else {
    obj.meta_ = meta;
    // defensive checking
    // if datum is decimal int, must satisfy scale >= 0
    if (OB_UNLIKELY(meta.is_decimal_int() && meta.get_scale() < 0)) {
      ret = OB_ERR_UNEXPECTED;
      COMMON_LOG(WARN, "invalid scale for decimal int", K(ret));
    } else {
      switch (map_type) {
        case OBJ_DATUM_NULL: { datum2obj<OBJ_DATUM_NULL>(obj); break; }
        case OBJ_DATUM_STRING: { datum2obj<OBJ_DATUM_STRING>(obj); break; }
        case OBJ_DATUM_NUMBER: { datum2obj<OBJ_DATUM_NUMBER>(obj); break; }
        case OBJ_DATUM_8BYTE_DATA: { datum2obj<OBJ_DATUM_8BYTE_DATA>(obj); break; }
        case OBJ_DATUM_4BYTE_DATA: { datum2obj<OBJ_DATUM_4BYTE_DATA>(obj); break; }
        case OBJ_DATUM_1BYTE_DATA: { datum2obj<OBJ_DATUM_1BYTE_DATA>(obj); break; }
        case OBJ_DATUM_4BYTE_LEN_DATA: { datum2obj<OBJ_DATUM_4BYTE_LEN_DATA>(obj); break; }
        case OBJ_DATUM_2BYTE_LEN_DATA: { datum2obj<OBJ_DATUM_2BYTE_LEN_DATA>(obj); break; }
        case OBJ_DATUM_EXTEND: { datum2obj<OBJ_DATUM_EXTEND>(obj); break; }
        case OBJ_DATUM_DECIMALINT: { datum2obj<OBJ_DATUM_DECIMALINT>(obj); break; }
        case OBJ_DATUM_MAPPING_MAX: {
          ret = common::OB_ERR_UNEXPECTED;
          COMMON_LOG(WARN, "invalid obj datum mapping", K(ret), K(map_type));
        }
      }
    }
  }
  return ret;
}

inline int ObDatum::to_obj(ObObj &obj, const ObObjMeta &meta) const
{
  int ret = common::OB_SUCCESS;
  if (is_null()) {
    // it exists datum is null, but meta type not nulltype, need keep cs_type attr
    obj.set_null();
    if (!meta.is_null()) {
      obj.set_collation_level(meta.get_collation_level());
      obj.set_collation_type(meta.get_collation_type());
    }
  } else {
    // defensive checking
    // if datum is decimal int, must satisfy scale >= 0
    if (OB_UNLIKELY(meta.is_decimal_int() && meta.get_scale() < 0)) {
      ret = OB_ERR_UNEXPECTED;
      COMMON_LOG(WARN, "invalid scale for decimal int", K(ret));
    } else {
      obj.meta_ = meta;
      switch (meta.get_type()) {
      case ObNullType: {
        datum2obj<OBJ_DATUM_NULL>(obj);
        break;
      }
      case ObVarcharType:
      case ObCharType:
      case ObHexStringType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType:
      case ObEnumInnerType:
      case ObSetInnerType:
      case ObJsonType:
      case ObGeometryType:
      case ObUserDefinedSQLType:
      case ObCollectionSQLType: {
        datum2obj<OBJ_DATUM_STRING>(obj);
        break;
      }
      case ObNumberType:
      case ObUNumberType: {
        datum2obj<OBJ_DATUM_NUMBER>(obj);
        break;
      }
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType:
      case ObUTinyIntType:
      case ObUSmallIntType:
      case ObUMediumIntType:
      case ObUInt32Type:
      case ObUInt64Type:
      case ObDoubleType:
      case ObUDoubleType:
      case ObDateTimeType:
      case ObTimestampType:
      case ObTimeType:
      case ObUnknownType:
      case ObBitType:
      case ObEnumType:
      case ObSetType:
      case ObMySQLDateTimeType: {
        datum2obj<OBJ_DATUM_8BYTE_DATA>(obj);
        break;
      }
      case ObFloatType:
      case ObUFloatType:
      case ObDateType:
      case ObMySQLDateType: {
        datum2obj<OBJ_DATUM_4BYTE_DATA>(obj);
        break;
      }
      case ObYearType: {
        datum2obj<OBJ_DATUM_1BYTE_DATA>(obj);
        break;
      }
      case ObExtendType: {
        datum2obj<OBJ_DATUM_EXTEND>(obj);
        break;
      }
      case ObDecimalIntType: {
        datum2obj<OBJ_DATUM_DECIMALINT>(obj);
        break;
      }
      case ObMaxType:
      default: {
        ret = common::OB_ERR_UNEXPECTED;
        COMMON_LOG(WARN, "invalid obj type", K(ret), K(meta.get_type()));
      }
      }
    }
  }
  return ret;
}

} // namespace common
} // namespace oceanbase
