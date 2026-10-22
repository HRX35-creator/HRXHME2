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
#include <type_traits>

#ifdef OCEANBASE_COMMON_OB_OBJECT_H_
#error "The native wire codec must not depend on the legacy value definition"
#endif

namespace oceanbase
{
namespace common
{
namespace
{
constexpr int MAX_ARRAY_DEPTH = 64;
uint32_t datum_length_limit()
{
  ObDatumDesc descriptor;
  descriptor.pack_ = UINT32_MAX;
  return descriptor.len_;
}


struct SizeArchive
{
  static constexpr bool reading = false;
  int ret_ = OB_SUCCESS;
  int64_t pos_ = 0;
  void add(int64_t length)
  {
    if (ret_ == OB_SUCCESS) {
      int64_t sum = 0;
      if (length < 0 || __builtin_add_overflow(pos_, length, &sum)) { ret_ = OB_SIZE_OVERFLOW; }
      else { pos_ = sum; }
    }
  }
  template <typename T> void field(const T &value)
  { if (ret_ == OB_SUCCESS) { add(serialization::encoded_length(value)); } }
  void raw(const void *bytes, int64_t length)
  {
    if (length > 0 && bytes == nullptr) { ret_ = OB_INVALID_ARGUMENT; }
    add(length);
  }
  void number(const number::ObNumber::Desc &desc, const uint32_t *)
  { add(serialization::encode_length_number_type(desc)); }
  void require(int64_t) {}
};

struct WriteArchive
{
  static constexpr bool reading = false;
  char *buf_;
  int64_t length_;
  int64_t pos_;
  int ret_ = OB_SUCCESS;
  WriteArchive(char *buf, int64_t length, int64_t pos) : buf_(buf), length_(length), pos_(pos) {}
  void require(int64_t length)
  {
    if (ret_ == OB_SUCCESS && (length < 0 || length > length_ - pos_)) {
      ret_ = OB_BUF_NOT_ENOUGH;
    }
  }
  template <typename T> void field(const T &value)
  { if (ret_ == OB_SUCCESS) { ret_ = serialization::encode(buf_, length_, pos_, value); } }
  void raw(const void *bytes, int64_t length)
  {
    require(length);
    if (ret_ == OB_SUCCESS && length > 0) {
      if (bytes == nullptr) { ret_ = OB_INVALID_ARGUMENT; }
      else { MEMCPY(buf_ + pos_, bytes, length); pos_ += length; }
    }
  }
  void number(const number::ObNumber::Desc &desc, const uint32_t *digits)
  {
    if (ret_ == OB_SUCCESS) {
      ret_ = serialization::encode_number_type(buf_, length_, pos_, desc, digits);
    }
  }
};

struct ReadArchive
{
  static constexpr bool reading = true;
  const char *buf_;
  int64_t length_;
  int64_t pos_;
  ObIAllocator &allocator_;
  int ret_ = OB_SUCCESS;
  ReadArchive(const char *buf, int64_t length, int64_t pos, ObIAllocator &allocator)
      : buf_(buf), length_(length), pos_(pos), allocator_(allocator) {}
  void require(int64_t length)
  {
    if (ret_ == OB_SUCCESS && (length < 0 || length > length_ - pos_)) {
      ret_ = OB_DESERIALIZE_ERROR;
    }
  }
  void field(ObString &value)
  {
    if (ret_ == OB_SUCCESS) {
      int64_t length = 0;
      ret_ = serialization::decode_vi64(buf_, length_, pos_, &length);
      // Check the native length limit before narrowing to ObString and require
      // the legacy terminator byte as well as the payload, without addition overflow.
      if (ret_ == OB_SUCCESS &&
          (length < 0 || length > datum_length_limit() || length >= length_ - pos_)) {
        ret_ = OB_DESERIALIZE_ERROR;
      }
      if (ret_ == OB_SUCCESS) { value.assign_ptr(buf_ + pos_, length); pos_ += length + 1; }
    }
  }
  template <typename T> void field(T &value)
  {
    require(1);
    if (ret_ == OB_SUCCESS) {
      const int64_t before = pos_;
      ret_ = serialization::decode(buf_, length_, pos_, value);
      if (ret_ == OB_SUCCESS && (pos_ <= before || pos_ > length_)) { ret_ = OB_DESERIALIZE_ERROR; }
    }
  }
  void allocate(ObDatum &value, int64_t length)
  {
    if (ret_ == OB_SUCCESS) {
      if (length < 0 || length > datum_length_limit()) { ret_ = OB_DESERIALIZE_ERROR; }
      else if ((value.ptr_ = static_cast<char *>(allocator_.alloc(std::max<int64_t>(length, 1)))) == nullptr) {
        ret_ = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  void copy(ObDatum &value, const char *bytes, int64_t length)
  {
    allocate(value, length);
    if (ret_ == OB_SUCCESS) {
      if (length > 0) { MEMCPY(const_cast<char *>(value.ptr_), bytes, length); }
      value.set_string(value.ptr_, length);
    }
  }
  void number(number::ObNumber::Desc &desc, const uint32_t *&digits)
  {
    uint32_t *borrowed = nullptr;
    if (ret_ == OB_SUCCESS) {
      ret_ = serialization::decode_number_type(buf_, length_, pos_, desc, borrowed);
      desc.reserved_ = 0;
      digits = borrowed;
    }
  }
};

template <typename Archive>
using ParameterView = std::conditional_t<Archive::reading, DatumParameter, const DatumParameter>;
template <typename Archive> void parameter(ParameterView<Archive> &, Archive &, int);

template <typename Archive>
void array_body(const DatumParameterArray &array, Archive &archive, int depth)
{
  archive.field(array.element_);
  archive.field(array.count_);
  for (int64_t i = 0; archive.ret_ == OB_SUCCESS && i < array.count_; ++i) {
    parameter(array.data_[i], archive, depth + 1);
  }
}

template <typename Archive, typename Wire, auto Getter, auto Setter>
void scalar(ObDatum &datum, Archive &archive)
{
  Wire value{};
  using Native = decltype((datum.*Getter)());
  if constexpr (!Archive::reading) {
    if (datum.ptr_ == nullptr || datum.len_ != sizeof(Native)) { archive.ret_ = OB_INVALID_ARGUMENT; }
    else { value = static_cast<Wire>((datum.*Getter)()); }
  }
  archive.field(value);
  if constexpr (Archive::reading) {
    archive.allocate(datum, sizeof(Native));
    if (archive.ret_ == OB_SUCCESS) { (datum.*Setter)(value); }
  }
}

template <typename Archive>
void value(ObDatum &datum, ObObjMeta &meta, Archive &archive, int depth)
{
  if (archive.ret_ != OB_SUCCESS) { return; }
  if (depth > MAX_ARRAY_DEPTH) { archive.ret_ = OB_SIZE_OVERFLOW; return; }
  if constexpr (!Archive::reading) {
    if (meta.is_invalid()) { archive.ret_ = OB_ERR_UNEXPECTED; return; }
    if (datum.is_null() && !meta.is_null()) { meta.set_null(); }
  }
  archive.require(4); // Fixed four metadata bytes; legacy macros alone allow truncated headers.
  archive.field(meta);
  if (archive.ret_ != OB_SUCCESS) { return; }
  if (meta.is_invalid()) { archive.ret_ = OB_ERR_UNEXPECTED; return; }
  switch (meta.get_type()) {
    case ObNullType: datum.set_null(); break;
    case ObTinyIntType: scalar<Archive, int8_t, &ObDatum::get_int, &ObDatum::set_int>(datum, archive); break;
    case ObSmallIntType: scalar<Archive, int16_t, &ObDatum::get_int, &ObDatum::set_int>(datum, archive); break;
    case ObMediumIntType: scalar<Archive, int32_t, &ObDatum::get_int, &ObDatum::set_int>(datum, archive); break;
    case ObInt32Type: scalar<Archive, int32_t, &ObDatum::get_int, &ObDatum::set_int>(datum, archive); break;
    case ObIntType: scalar<Archive, int64_t, &ObDatum::get_int, &ObDatum::set_int>(datum, archive); break;
    case ObUTinyIntType: scalar<Archive, uint8_t, &ObDatum::get_uint, &ObDatum::set_uint>(datum, archive); break;
    case ObUSmallIntType: scalar<Archive, uint16_t, &ObDatum::get_uint, &ObDatum::set_uint>(datum, archive); break;
    case ObUMediumIntType: scalar<Archive, uint32_t, &ObDatum::get_uint, &ObDatum::set_uint>(datum, archive); break;
    case ObUInt32Type: scalar<Archive, uint32_t, &ObDatum::get_uint, &ObDatum::set_uint>(datum, archive); break;
    case ObUInt64Type: scalar<Archive, uint64_t, &ObDatum::get_uint, &ObDatum::set_uint>(datum, archive); break;
    case ObFloatType: scalar<Archive, float, &ObDatum::get_float, &ObDatum::set_float>(datum, archive); break;
    case ObUFloatType: scalar<Archive, float, &ObDatum::get_float, &ObDatum::set_float>(datum, archive); break;
    case ObDoubleType: scalar<Archive, double, &ObDatum::get_double, &ObDatum::set_double>(datum, archive); break;
    case ObUDoubleType: scalar<Archive, double, &ObDatum::get_double, &ObDatum::set_double>(datum, archive); break;
    case ObDateTimeType: scalar<Archive, int64_t, &ObDatum::get_datetime, &ObDatum::set_datetime>(datum, archive); break;
    case ObTimestampType: scalar<Archive, int64_t, &ObDatum::get_timestamp, &ObDatum::set_timestamp>(datum, archive); break;
    case ObDateType: scalar<Archive, int32_t, &ObDatum::get_date, &ObDatum::set_date>(datum, archive); break;
    case ObTimeType: scalar<Archive, int64_t, &ObDatum::get_time, &ObDatum::set_time>(datum, archive); break;
    case ObYearType: scalar<Archive, uint8_t, &ObDatum::get_year, &ObDatum::set_year>(datum, archive); break;
    case ObBitType: scalar<Archive, uint64_t, &ObDatum::get_bit, &ObDatum::set_bit>(datum, archive); break;
    case ObEnumType: scalar<Archive, uint64_t, &ObDatum::get_enum, &ObDatum::set_enum>(datum, archive); break;
    case ObSetType: scalar<Archive, uint64_t, &ObDatum::get_set, &ObDatum::set_set>(datum, archive); break;
    case ObUnknownType: scalar<Archive, int64_t, &ObDatum::get_unknown, &ObDatum::set_int>(datum, archive); break;
    case ObMySQLDateType: scalar<Archive, int32_t, &ObDatum::get_date, &ObDatum::set_date>(datum, archive); break;
    case ObMySQLDateTimeType: scalar<Archive, int64_t, &ObDatum::get_datetime, &ObDatum::set_datetime>(datum, archive); break;
    case ObNumberType:
    case ObUNumberType: {
      number::ObNumber::Desc desc{};
      const uint32_t *digits = nullptr;
      if constexpr (!Archive::reading) {
        if (datum.ptr_ == nullptr || datum.len_ < sizeof(desc)) { archive.ret_ = OB_INVALID_ARGUMENT; }
        else {
          desc = datum.get_number_desc();
          if (datum.len_ != sizeof(desc) + sizeof(uint32_t) * desc.len_) { archive.ret_ = OB_INVALID_ARGUMENT; }
          else { digits = datum.get_number_digits(); }
          desc.reserved_ = 0;
          desc.flag_ = 0;
        }
      }
      if (archive.ret_ == OB_SUCCESS) { archive.number(desc, digits); }
      if constexpr (Archive::reading) {
        const int64_t length = sizeof(desc) + sizeof(uint32_t) * desc.len_;
        archive.allocate(datum, length);
        if (archive.ret_ == OB_SUCCESS) {
          char *bytes = const_cast<char *>(datum.ptr_);
          MEMCPY(bytes, &desc, sizeof(desc));
          if (desc.len_ > 0) { MEMCPY(bytes + sizeof(desc), digits, sizeof(uint32_t) * desc.len_); }
          datum.set_string(bytes, length);
        }
      }
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
      if (meta.get_type() == ObUserDefinedSQLType || meta.is_collection_sql_type()) {
        uint16_t subschema_id = meta.get_subschema_id();
        uint8_t flags = meta.get_udt_flags();
        archive.field(subschema_id);
        archive.field(flags);
        if constexpr (Archive::reading) { meta.set_subschema_id(subschema_id); meta.set_udt_flags(flags); }
      }
      ObString bytes;
      if constexpr (!Archive::reading) {
        if (datum.len_ > INT32_MAX || (datum.len_ > 0 && datum.ptr_ == nullptr)) { archive.ret_ = OB_INVALID_ARGUMENT; }
        else { bytes = datum.get_string(); }
      }
      archive.field(bytes);
      if constexpr (Archive::reading) {
        archive.copy(datum, bytes.ptr(), bytes.length());
        if (meta.is_outrow()) { datum.set_outrow(); }
        else if (meta.has_lob_header()) { datum.set_has_lob_header(); }
      }
      break;
    }
    case ObDecimalIntType: {
      int32_t length = 0;
      ObScale scale = meta.get_scale();
      if constexpr (!Archive::reading) {
        if (datum.len_ > sizeof(int512_t)) { archive.ret_ = OB_INVALID_ARGUMENT; }
        else { length = datum.len_; }
      }
      archive.field(length); archive.field(scale);
      if constexpr (Archive::reading) {
        if (length < 0 || length > sizeof(int512_t)) { archive.ret_ = OB_DESERIALIZE_ERROR; }
        archive.require(length);
        if (archive.ret_ == OB_SUCCESS) {
          archive.copy(datum, archive.buf_ + archive.pos_, length);
          archive.pos_ += length;
          meta.set_decimal_int(scale);
        }
      } else { archive.raw(datum.ptr_, length); }
      break;
    }
    case ObExtendType: {
      if (meta.is_ext_sql_array()) {
        if constexpr (Archive::reading) {
          int64_t zero = 0, length = 0;
          archive.field(zero); archive.field(length);
          if (archive.ret_ == OB_SUCCESS && zero != 0) { archive.ret_ = OB_NOT_SUPPORTED; }
          archive.require(length);
          if (archive.ret_ == OB_SUCCESS) {
            const int64_t end = archive.pos_ + length;
            const int64_t parent_end = archive.length_;
            archive.length_ = end;
            ObDataType element;
            int64_t count = 0;
            archive.field(element); archive.field(count);
            // Every parameter has at least four metadata bytes and two scalar fields.
            if (archive.ret_ == OB_SUCCESS && (count < 0 || count > (end - archive.pos_) / 6)) {
              archive.ret_ = OB_DESERIALIZE_ERROR;
            }
            DatumParameterArray *array = nullptr;
            if (archive.ret_ == OB_SUCCESS &&
                (array = DatumParameterArray::alloc(archive.allocator_, count)) == nullptr) {
              archive.ret_ = OB_ALLOCATE_MEMORY_FAILED;
            }
            if (archive.ret_ == OB_SUCCESS) { array->element_ = element; }
            for (int64_t i = 0; archive.ret_ == OB_SUCCESS && i < count; ++i) {
              parameter(array->data_[i], archive, depth + 1);
            }
            if (archive.ret_ == OB_SUCCESS && archive.pos_ != end) { archive.ret_ = OB_DESERIALIZE_ERROR; }
            archive.length_ = parent_end;
            archive.allocate(datum, sizeof(int64_t));
            if (archive.ret_ == OB_SUCCESS) { datum.set_int(reinterpret_cast<int64_t>(array)); }
          }
        } else {
          if (datum.ptr_ == nullptr || datum.len_ != sizeof(int64_t)) { archive.ret_ = OB_INVALID_ARGUMENT; break; }
          const auto *array = reinterpret_cast<const DatumParameterArray *>(datum.get_int());
          if (array == nullptr || array->count_ < 0 || (array->count_ > 0 && array->data_ == nullptr)) {
            archive.ret_ = OB_INVALID_ARGUMENT; break;
          }
          SizeArchive size;
          array_body(*array, size, depth);
          if (size.ret_ != OB_SUCCESS) { archive.ret_ = size.ret_; break; }
          const int64_t zero = 0;
          archive.field(zero); archive.field(size.pos_);
          if constexpr (std::is_same_v<Archive, SizeArchive>) { archive.add(size.pos_); }
          else { array_body(*array, archive, depth); }
        }
      } else {
        int64_t extended = 0;
        if constexpr (!Archive::reading) {
          if (datum.ptr_ == nullptr || !datum.has_ext_value()) { archive.ret_ = OB_INVALID_ARGUMENT; }
          else {
            extended = datum.get_ext();
            const bool sentinel = (extended >= ObActionFlag::OP_MIN_OBJ && extended <= ObActionFlag::OP_LOCK_ROW)
                || extended == DATUM_MIN_EXT_VALUE || extended == DATUM_MAX_EXT_VALUE;
            if (meta.is_pl_extend_type() && !sentinel) { archive.ret_ = OB_NOT_SUPPORTED; }
          }
        }
        archive.field(extended);
        if constexpr (Archive::reading) {
          const bool sentinel = (extended >= ObActionFlag::OP_MIN_OBJ && extended <= ObActionFlag::OP_LOCK_ROW)
              || extended == DATUM_MIN_EXT_VALUE || extended == DATUM_MAX_EXT_VALUE || extended == 0;
          if (archive.ret_ == OB_SUCCESS && !sentinel) { archive.ret_ = OB_NOT_SUPPORTED; }
          archive.allocate(datum, DATUM_EXTEND_PAYLOAD_SIZE);
          if (archive.ret_ == OB_SUCCESS) {
            datum.set_extend(extended, meta.get_extend_type(), 0, meta.get_cs_level(), meta.get_cs_type());
          }
        }
      }
      break;
    }
    default: archive.ret_ = OB_NOT_SUPPORTED; break;
  }
}

template <typename Archive>
void parameter(ParameterView<Archive> &parameter_value, Archive &archive, int depth)
{
  ObDatum datum(parameter_value.datum_);
  ObObjMeta meta(parameter_value.get_type_meta());
  value(datum, meta, archive, depth);
  ObAccuracy accuracy(parameter_value.get_accuracy());
  uint32_t flags = parameter_value.get_result_flag();
  archive.field(accuracy); archive.field(flags);
  if constexpr (Archive::reading) {
    if (archive.ret_ == OB_SUCCESS) {
      parameter_value.set_datum(datum);
      parameter_value.set_type_meta(meta);
      parameter_value.set_accuracy(accuracy);
      parameter_value.set_result_flags(flags);
    }
  }
}

bool valid_buffer(const void *buf, int64_t length, int64_t pos)
{ return buf != nullptr && pos >= 0 && length >= pos; }

} // anonymous namespace

int DatumParameterCodec::store_size(const DatumParamStore &store, int64_t &size)
{
  SizeArchive archive;
  archive.field(store.count());
  for (int64_t i = 0; archive.ret_ == OB_SUCCESS && i < store.count(); ++i) {
    parameter(store.at(i), archive, 0);
  }
  if (archive.ret_ == OB_SUCCESS) { size = archive.pos_; }
  return archive.ret_;
}

int DatumParameterCodec::encode_store(const DatumParamStore &store, char *buf,
                                      int64_t length, int64_t &pos)
{
  if (!valid_buffer(buf, length, pos)) { return OB_INVALID_ARGUMENT; }
  WriteArchive archive(buf, length, pos);
  archive.field(store.count());
  for (int64_t i = 0; archive.ret_ == OB_SUCCESS && i < store.count(); ++i) {
    parameter(store.at(i), archive, 0);
  }
  if (archive.ret_ == OB_SUCCESS) { pos = archive.pos_; }
  return archive.ret_;
}

int DatumParameterCodec::decode_store(const char *buf, int64_t length, int64_t &pos,
                                      ObIAllocator &allocator, DatumParamStore &store)
{
  store.reuse();
  if (!valid_buffer(buf, length, pos)) { return OB_INVALID_ARGUMENT; }
  ReadArchive archive(buf, length, pos, allocator);
  int64_t count = 0;
  archive.field(count);
  if (archive.ret_ == OB_SUCCESS && (count < 0 || count > (length - archive.pos_) / 6)) {
    archive.ret_ = OB_DESERIALIZE_ERROR;
  }
  if (archive.ret_ == OB_SUCCESS) { archive.ret_ = store.prepare_allocate(count); }
  for (int64_t i = 0; archive.ret_ == OB_SUCCESS && i < count; ++i) {
    parameter(store.at(i), archive, 0);
  }
  if (archive.ret_ == OB_SUCCESS) { pos = archive.pos_; }
  else { store.reuse(); }
  return archive.ret_;
}

int DatumParameterCodec::value_size(const ObDatum &datum, const ObObjMeta &meta, int64_t &size)
{
  ObDatum view(datum);
  ObObjMeta type(meta);
  SizeArchive archive;
  value(view, type, archive, 0);
  if (archive.ret_ == OB_SUCCESS) { size = archive.pos_; }
  return archive.ret_;
}

int DatumParameterCodec::encode_value(const ObDatum &datum, const ObObjMeta &meta,
                                      char *buf, int64_t length, int64_t &pos)
{
  if (!valid_buffer(buf, length, pos)) { return OB_INVALID_ARGUMENT; }
  ObDatum view(datum);
  ObObjMeta type(meta);
  WriteArchive archive(buf, length, pos);
  value(view, type, archive, 0);
  if (archive.ret_ == OB_SUCCESS) { pos = archive.pos_; }
  return archive.ret_;
}

int DatumParameterCodec::decode_value(const char *buf, int64_t length, int64_t &pos,
                                      ObIAllocator &allocator, ObDatum &datum, ObObjMeta &meta)
{
  if (!valid_buffer(buf, length, pos)) { return OB_INVALID_ARGUMENT; }
  ObDatum output;
  output.set_null();
  ObObjMeta type;
  ReadArchive archive(buf, length, pos, allocator);
  value(output, type, archive, 0);
  if (archive.ret_ == OB_SUCCESS) { datum = output; meta = type; pos = archive.pos_; }
  return archive.ret_;
}

int DatumParameterCodec::parameter_size(const DatumParameter &source, int64_t &size)
{
  SizeArchive archive;
  parameter(source, archive, 0);
  if (archive.ret_ == OB_SUCCESS) { size = archive.pos_; }
  return archive.ret_;
}

int DatumParameterCodec::encode_parameter(const DatumParameter &source, char *buf,
                                          int64_t length, int64_t &pos)
{
  if (!valid_buffer(buf, length, pos)) { return OB_INVALID_ARGUMENT; }
  WriteArchive archive(buf, length, pos);
  parameter(source, archive, 0);
  if (archive.ret_ == OB_SUCCESS) { pos = archive.pos_; }
  return archive.ret_;
}

int DatumParameterCodec::decode_parameter(const char *buf, int64_t length, int64_t &pos,
                                          ObIAllocator &allocator, DatumParameter &output)
{
  if (!valid_buffer(buf, length, pos)) { return OB_INVALID_ARGUMENT; }
  DatumParameter candidate;
  ReadArchive archive(buf, length, pos, allocator);
  parameter(candidate, archive, 0);
  if (archive.ret_ == OB_SUCCESS) { output = candidate; pos = archive.pos_; }
  return archive.ret_;
}

} // namespace common
} // namespace oceanbase
