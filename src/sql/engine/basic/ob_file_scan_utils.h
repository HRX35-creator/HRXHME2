#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"
#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "common/object/ob_object.h"

#include "rapidjson/document.h"

namespace oceanbase
{
namespace sql
{

enum class ObFileSqlFormat : int8_t
{
  CSV = 0,
  JSONL = 1,
  NDJSON = 2,
  PARQUET = 3,
  GLOB = 4,
};

struct ObFileSqlColumn
{
  common::ObString name_;
  common::ObObjType type_;
  bool nullable_;
  int32_t precision_;
  int8_t scale_;
};

struct ObFileSqlOptions
{
  ObFileSqlOptions()
      : header_(true),
        delimiter_(','),
        quote_('"'),
        sample_size_(-1),
        max_file_size_(1024LL * 1024LL * 1024LL),
        max_rows_(10000000),
        null_text_()
  {
  }
  bool header_;
  char delimiter_;
  char quote_;
  int64_t sample_size_;
  int64_t max_file_size_;
  int64_t max_rows_;
  common::ObString null_text_;
};

class ObFileSqlUtils
{
public:
  static int infer_schema(common::ObIAllocator &allocator,
                          const common::ObString &path,
                          ObFileSqlFormat format,
                          const ObFileSqlOptions &options,
                          common::ObArray<ObFileSqlColumn> &columns,
                          common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    if (format == ObFileSqlFormat::CSV) {
      ret = infer_csv_schema_(allocator, path, options, columns, error_msg);
    } else if (format == ObFileSqlFormat::JSONL || format == ObFileSqlFormat::NDJSON) {
      ret = infer_jsonl_schema_(allocator, path, columns, error_msg);
    } else if (format == ObFileSqlFormat::PARQUET) {
      ret = infer_parquet_schema_(allocator, path, columns, error_msg);
    } else {
      ret = common::OB_NOT_SUPPORTED;
      error_msg = common::ObString("glob schema inference is not implemented yet");
    }
    return ret;
  }

private:
  enum class InferType : int8_t
  {
    UNKNOWN,
    BIGINT,
    DOUBLE,
    BOOLEAN,
    VARCHAR,
    DATE,
    DATETIME,
  };

  static bool is_null_value_(const std::string &value, const common::ObString &null_text)
  {
    return !null_text.empty() && null_text.length() == static_cast<int32_t>(value.size()) &&
           memcmp(null_text.ptr(), value.data(), value.size()) == 0;
  }

  static bool is_integer_(const std::string &value)
  {
    if (value.empty()) {
      return false;
    }
    size_t pos = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    if (pos >= value.size()) {
      return false;
    }
    for (size_t i = pos; i < value.size(); ++i) {
      if (value[i] < '0' || value[i] > '9') {
        return false;
      }
    }
    return true;
  }

  static bool is_double_(const std::string &value)
  {
    if (value.empty()) {
      return false;
    }
    char *end = nullptr;
    strtod(value.c_str(), &end);
    return end != value.c_str() && *end == '\0';
  }

  static bool is_boolean_(const std::string &value)
  {
    return value == "true" || value == "false" || value == "TRUE" || value == "FALSE";
  }

  static bool is_date_(const std::string &value)
  {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
      return false;
    }
    for (int i = 0; i < 10; ++i) {
      if (i == 4 || i == 7) {
        continue;
      }
      if (value[i] < '0' || value[i] > '9') {
        return false;
      }
    }
    return true;
  }

  static bool is_datetime_(const std::string &value)
  {
    if (value.size() != 19 || value[4] != '-' || value[7] != '-' ||
        value[10] != ' ' || value[13] != ':' || value[16] != ':') {
      return false;
    }
    for (int i = 0; i < 19; ++i) {
      if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
        continue;
      }
      if (value[i] < '0' || value[i] > '9') {
        return false;
      }
    }
    return true;
  }

  static InferType infer_type_(const std::string &value, const common::ObString &null_text)
  {
    if (is_null_value_(value, null_text)) {
      return InferType::UNKNOWN;
    } else if (is_boolean_(value)) {
      return InferType::BOOLEAN;
    } else if (is_integer_(value)) {
      return InferType::BIGINT;
    } else if (is_double_(value)) {
      return InferType::DOUBLE;
    } else if (is_date_(value)) {
      return InferType::DATE;
    } else if (is_datetime_(value)) {
      return InferType::DATETIME;
    } else {
      return InferType::VARCHAR;
    }
  }

  static InferType merge_type_(InferType lhs, InferType rhs)
  {
    if (lhs == InferType::UNKNOWN) {
      return rhs;
    }
    if (rhs == InferType::UNKNOWN) {
      return lhs;
    }
    if (lhs == rhs) {
      return lhs;
    }
    if (lhs == InferType::BIGINT && rhs == InferType::DOUBLE) {
      return InferType::DOUBLE;
    }
    if (lhs == InferType::DOUBLE && rhs == InferType::BIGINT) {
      return InferType::DOUBLE;
    }
    return InferType::VARCHAR;
  }

  static common::ObObjType to_obj_type_(InferType type)
  {
    switch (type) {
      case InferType::BIGINT:
        return common::ObIntType;
      case InferType::DOUBLE:
        return common::ObDoubleType;
      case InferType::BOOLEAN:
        return common::ObTinyIntType;
      case InferType::DATE:
        return common::ObDateType;
      case InferType::DATETIME:
        return common::ObDateTimeType;
      case InferType::VARCHAR:
      case InferType::UNKNOWN:
      default:
        return common::ObVarcharType;
    }
  }

  static int infer_csv_schema_(common::ObIAllocator &allocator,
                               const common::ObString &path,
                               const ObFileSqlOptions &options,
                               common::ObArray<ObFileSqlColumn> &columns,
                               common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    std::ifstream in(std::string(path.ptr(), path.length()), std::ios::binary);
    if (!in.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      error_msg = common::ObString("failed to open csv file");
      return ret;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    std::vector<std::vector<std::string>> records;
    if (OB_FAIL(parse_csv_records_(content, options, records))) {
      error_msg = common::ObString("malformed csv record");
      return ret;
    }
    if (records.empty()) {
      ret = common::OB_INVALID_DATA;
      error_msg = common::ObString("csv file is empty");
      return ret;
    }

    const int64_t column_count = records[0].size();
    if (column_count == 0) {
      ret = common::OB_INVALID_DATA;
      error_msg = common::ObString("csv header is empty");
      return ret;
    }

    std::vector<InferType> types(column_count, InferType::UNKNOWN);
    std::vector<std::string> names(column_count);
    for (int64_t i = 0; i < column_count; ++i) {
      names[i] = options.header_ ? records[0][i] : "c" + std::to_string(i + 1);
    }

    const int64_t first_data_row = options.header_ ? 1 : 0;
    for (int64_t r = first_data_row; OB_SUCC(ret) && r < static_cast<int64_t>(records.size()); ++r) {
      if (records[r].size() != static_cast<size_t>(column_count)) {
        ret = common::OB_INVALID_DATA;
        error_msg = common::ObString("csv row has a different number of fields");
        break;
      }
      for (int64_t c = 0; c < column_count; ++c) {
        types[c] = merge_type_(types[c], infer_type_(records[r][c], options.null_text_));
      }
    }

    for (int64_t c = 0; OB_SUCC(ret) && c < column_count; ++c) {
      ObFileSqlColumn column;
      common::ObString name;
      if (OB_FAIL(ob_write_string(allocator, common::ObString(names[c].size(), names[c].data()), name))) {
      } else {
        column.name_ = name;
        column.type_ = to_obj_type_(types[c]);
        column.nullable_ = true;
        column.precision_ = -1;
        column.scale_ = -1;
        if (OB_FAIL(columns.push_back(column))) {
        }
      }
    }
    return ret;
  }

  static int infer_jsonl_schema_(common::ObIAllocator &allocator,
                                 const common::ObString &path,
                                 common::ObArray<ObFileSqlColumn> &columns,
                                 common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    std::ifstream in(std::string(path.ptr(), path.length()), std::ios::binary);
    if (!in.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      error_msg = common::ObString("failed to open jsonl file");
      return ret;
    }

    std::vector<std::string> names;
    std::vector<InferType> types;
    std::string line;
    int64_t row_no = 0;
    while (OB_SUCC(ret) && std::getline(in, line)) {
      ++row_no;
      if (line.empty()) {
        continue;
      }
      rapidjson::Document doc;
      doc.Parse(line.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        ret = common::OB_INVALID_DATA;
        error_msg = common::ObString("jsonl row is not a flat object");
        break;
      }
      for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin();
           it != doc.MemberEnd(); ++it) {
        std::string key = it->name.GetString();
        const rapidjson::Value &value = it->value;
        if (value.IsObject() || value.IsArray()) {
          ret = common::OB_NOT_SUPPORTED;
          error_msg = common::ObString("nested json objects/arrays are not supported");
          break;
        }
        int64_t idx = -1;
        for (int64_t i = 0; i < static_cast<int64_t>(names.size()); ++i) {
          if (names[i] == key) {
            idx = i;
            break;
          }
        }
        if (idx < 0) {
          idx = names.size();
          names.push_back(key);
          types.push_back(InferType::UNKNOWN);
        }
        InferType next = InferType::VARCHAR;
        if (value.IsNull()) {
          next = InferType::UNKNOWN;
        } else if (value.IsInt64()) {
          next = InferType::BIGINT;
        } else if (value.IsDouble()) {
          next = InferType::DOUBLE;
        } else if (value.IsBool()) {
          next = InferType::BOOLEAN;
        } else if (value.IsString()) {
          next = InferType::VARCHAR;
        }
        types[idx] = merge_type_(types[idx], next);
      }
    }
    in.close();

    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(names.size()); ++i) {
      ObFileSqlColumn column;
      common::ObString name;
      if (OB_FAIL(ob_write_string(allocator, common::ObString(names[i].size(), names[i].data()), name))) {
      } else {
        column.name_ = name;
        column.type_ = to_obj_type_(types[i]);
        column.nullable_ = true;
        column.precision_ = -1;
        column.scale_ = -1;
        if (OB_FAIL(columns.push_back(column))) {
        }
      }
    }
    return ret;
  }

  class ParquetCompactReader
  {
  public:
    ParquetCompactReader(const char *data, int64_t len)
        : data_(reinterpret_cast<const uint8_t *>(data)),
          len_(len),
          pos_(0)
    {
    }

    int read_byte(uint8_t &value)
    {
      int ret = common::OB_SUCCESS;
      if (pos_ >= len_) {
        ret = common::OB_INVALID_DATA;
      } else {
        value = data_[pos_++];
      }
      return ret;
    }

    int read_varint(uint64_t &value)
    {
      int ret = common::OB_SUCCESS;
      value = 0;
      int shift = 0;
      while (OB_SUCC(ret)) {
        uint8_t byte = 0;
        if (shift >= 64) {
          ret = common::OB_INVALID_DATA;
        } else if (OB_FAIL(read_byte(byte))) {
        } else {
          value |= static_cast<uint64_t>(byte & 0x7f) << shift;
          if ((byte & 0x80) == 0) {
            break;
          }
          shift += 7;
        }
      }
      return ret;
    }

    int read_zigzag(int64_t &value)
    {
      int ret = common::OB_SUCCESS;
      uint64_t raw = 0;
      if (OB_FAIL(read_varint(raw))) {
      } else {
        value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
      }
      return ret;
    }

    int read_field_header(uint8_t &type, int16_t &field_id, int16_t &last_id)
    {
      int ret = common::OB_SUCCESS;
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else if (0 == byte) {
        type = 0;
      } else {
        type = byte & 0x0f;
        uint8_t delta = byte >> 4;
        if (0 == delta) {
          int64_t id = 0;
          if (OB_FAIL(read_zigzag(id))) {
          } else {
            field_id = static_cast<int16_t>(id);
          }
        } else {
          field_id = last_id + delta;
        }
        last_id = field_id;
      }
      return ret;
    }

    int read_binary(std::string &value)
    {
      int ret = common::OB_SUCCESS;
      uint64_t len = 0;
      if (OB_FAIL(read_varint(len)) || len > static_cast<uint64_t>(len_ - pos_)) {
        ret = common::OB_INVALID_DATA;
      } else {
        value.assign(reinterpret_cast<const char *>(data_ + pos_), len);
        pos_ += len;
      }
      return ret;
    }

    int read_list_header(uint8_t &elem_type, uint64_t &size)
    {
      int ret = common::OB_SUCCESS;
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else {
        elem_type = byte & 0x0f;
        size = byte >> 4;
        if (15 == size) {
          ret = read_varint(size);
        }
      }
      return ret;
    }

    int skip_value(uint8_t type)
    {
      int ret = common::OB_SUCCESS;
      if (1 == type || 2 == type) {
      } else if (3 == type) {
        uint8_t value = 0;
        ret = read_byte(value);
      } else if (4 == type || 5 == type || 6 == type) {
        int64_t value = 0;
        ret = read_zigzag(value);
      } else if (7 == type) {
        if (pos_ + 8 > len_) {
          ret = common::OB_INVALID_DATA;
        } else {
          pos_ += 8;
        }
      } else if (8 == type) {
        std::string value;
        ret = read_binary(value);
      } else if (9 == type || 10 == type) {
        uint8_t elem_type = 0;
        uint64_t size = 0;
        if (OB_FAIL(read_list_header(elem_type, size))) {
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            ret = skip_value(elem_type);
          }
        }
      } else if (11 == type) {
        uint64_t size = 0;
        uint8_t key_type = 0;
        uint8_t value_type = 0;
        if (OB_FAIL(read_varint(size))) {
        } else if (size > 0) {
          uint8_t byte = 0;
          if (OB_FAIL(read_byte(byte))) {
          } else {
            key_type = byte >> 4;
            value_type = byte & 0x0f;
            for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
              if (OB_FAIL(skip_value(key_type))) {
              } else {
                ret = skip_value(value_type);
              }
            }
          }
        }
      } else if (12 == type) {
        int16_t last_id = 0;
        while (OB_SUCC(ret)) {
          uint8_t field_type = 0;
          int16_t field_id = 0;
          if (OB_FAIL(read_field_header(field_type, field_id, last_id))) {
          } else if (0 == field_type) {
            break;
          } else {
            ret = skip_value(field_type);
          }
        }
      } else {
        ret = common::OB_INVALID_DATA;
      }
      return ret;
    }

  private:
    const uint8_t *data_;
    int64_t len_;
    int64_t pos_;
  };

  static int parquet_type_to_obj_type_(int64_t physical_type,
                                       int64_t converted_type,
                                       common::ObObjType &obj_type)
  {
    int ret = common::OB_SUCCESS;
    switch (physical_type) {
      case 0:
        obj_type = common::ObTinyIntType;
        break;
      case 1:
        obj_type = (6 == converted_type) ? common::ObDateType : common::ObIntType;
        break;
      case 2:
        obj_type = (9 == converted_type || 10 == converted_type) ?
                   common::ObDateTimeType : common::ObIntType;
        break;
      case 3:
        obj_type = common::ObDateTimeType;
        break;
      case 4:
        obj_type = common::ObFloatType;
        break;
      case 5:
        obj_type = common::ObDoubleType;
        break;
      case 6:
      case 7:
        obj_type = common::ObVarcharType;
        break;
      default:
        ret = common::OB_NOT_SUPPORTED;
        break;
    }
    return ret;
  }

  static int parse_parquet_schema_element_(ParquetCompactReader &reader,
                                           int64_t &physical_type,
                                           int64_t &converted_type,
                                           std::string &name,
                                           int64_t &num_children)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    physical_type = -1;
    converted_type = -1;
    num_children = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 3 == field_id || 5 == field_id || 6 == field_id) &&
                 (5 == type || 4 == type || 3 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (1 == field_id) {
          physical_type = value;
        } else if (5 == field_id) {
          num_children = value;
        } else if (6 == field_id) {
          converted_type = value;
        }
      } else if (4 == field_id && 8 == type) {
        if (OB_FAIL(reader.read_binary(name))) {
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  static int parse_parquet_footer_(ParquetCompactReader &reader,
                                   common::ObIAllocator &allocator,
                                   common::ObArray<ObFileSqlColumn> &columns,
                                   common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (2 == field_id && 9 == type) {
        uint8_t elem_type = 0;
        uint64_t schema_count = 0;
        if (OB_FAIL(reader.read_list_header(elem_type, schema_count))) {
        } else if (12 != elem_type) {
          ret = common::OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < schema_count; ++i) {
            int64_t physical_type = -1;
            int64_t converted_type = -1;
            int64_t num_children = 0;
            std::string name;
            if (OB_FAIL(parse_parquet_schema_element_(reader, physical_type,
                                                       converted_type, name,
                                                       num_children))) {
            } else if (physical_type < 0) {
              // group/root node
            } else {
              ObFileSqlColumn column;
              common::ObString column_name;
              if (OB_FAIL(parquet_type_to_obj_type_(physical_type, converted_type,
                                                     column.type_))) {
                error_msg = common::ObString("unsupported parquet physical type");
              } else if (OB_FAIL(ob_write_string(
                             allocator,
                             common::ObString(name.size(), name.data()),
                             column_name))) {
              } else {
                column.name_ = column_name;
                column.nullable_ = true;
                column.precision_ = -1;
                column.scale_ = -1;
                if (OB_FAIL(columns.push_back(column))) {
                }
              }
            }
          }
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  static int infer_parquet_schema_(common::ObIAllocator &allocator,
                                   const common::ObString &path,
                                   common::ObArray<ObFileSqlColumn> &columns,
                                   common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    std::ifstream in(std::string(path.ptr(), path.length()), std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      error_msg = common::ObString("failed to open parquet file");
    } else {
      std::streamoff file_size = in.tellg();
      if (file_size < 12) {
        ret = common::OB_INVALID_DATA;
        error_msg = common::ObString("parquet file is too small");
      } else {
        char magic[4] = {0};
        in.seekg(0, std::ios::beg);
        in.read(magic, 4);
        if (0 != memcmp(magic, "PAR1", 4)) {
          ret = common::OB_INVALID_DATA;
          error_msg = common::ObString("invalid parquet magic");
        } else {
          in.seekg(file_size - 8, std::ios::beg);
          uint8_t footer_len_buf[4] = {0};
          in.read(reinterpret_cast<char *>(footer_len_buf), 4);
          uint32_t footer_len = static_cast<uint8_t>(footer_len_buf[0]) |
                                (static_cast<uint32_t>(footer_len_buf[1]) << 8) |
                                (static_cast<uint32_t>(footer_len_buf[2]) << 16) |
                                (static_cast<uint32_t>(footer_len_buf[3]) << 24);
          char end_magic[4] = {0};
          in.read(end_magic, 4);
          if (0 != memcmp(end_magic, "PAR1", 4) ||
              static_cast<std::streamoff>(footer_len) > file_size - 12) {
            ret = common::OB_INVALID_DATA;
            error_msg = common::ObString("invalid parquet footer");
          } else {
            std::string footer(footer_len, '\0');
            in.seekg(file_size - 8 - footer_len, std::ios::beg);
            in.read(&footer[0], footer_len);
            ParquetCompactReader reader(footer.data(), footer.size());
            if (OB_FAIL(parse_parquet_footer_(reader, allocator, columns, error_msg))) {
            }
          }
        }
      }
    }
    in.close();
    return ret;
  }

  static int parse_csv_records_(const std::string &content,
                                const ObFileSqlOptions &options,
                                std::vector<std::vector<std::string>> &records)
  {
    int ret = common::OB_SUCCESS;
    std::vector<std::string> record;
    std::string field;
    bool in_quotes = false;
    const int64_t len = content.size();
    for (int64_t i = 0; i < len; ++i) {
      char ch = content[i];
      if (in_quotes) {
        if (ch == options.quote_) {
          if (i + 1 < len && content[i + 1] == options.quote_) {
            field.push_back(options.quote_);
            ++i;
          } else {
            in_quotes = false;
          }
        } else {
          field.push_back(ch);
        }
      } else if (ch == options.quote_) {
        in_quotes = true;
      } else if (ch == options.delimiter_) {
        record.push_back(field);
        field.clear();
      } else if (ch == '\n') {
        record.push_back(field);
        field.clear();
        records.push_back(record);
        record.clear();
      } else if (ch == '\r') {
        if (i + 1 < len && content[i + 1] == '\n') {
          continue;
        }
      } else {
        field.push_back(ch);
      }
    }
    if (!field.empty() || !record.empty()) {
      record.push_back(field);
      records.push_back(record);
    }
    return ret;
  }
};

class ObFileCsvReader
{
public:
  ObFileCsvReader()
      : in_(),
        options_(),
        peek_(-1),
        eof_(true),
        stat_valid_(false),
        stat_info_(),
        row_count_(0)
  {
  }

  ~ObFileCsvReader()
  {
    if (in_.is_open()) {
      in_.close();
    }
  }

  int open(const common::ObString &path, const ObFileSqlOptions &options)
  {
    int ret = common::OB_SUCCESS;
    options_ = options;
    peek_ = -1;
    eof_ = false;
    stat_valid_ = false;
    row_count_ = 0;
    in_.clear();
    std::string path_str(path.ptr(), path.length());
    path_ = path_str;
    in_.open(path_str, std::ios::binary);
    if (!in_.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      eof_ = true;
    } else if (0 == stat(path_str.c_str(), &stat_info_)) {
      stat_valid_ = true;
      if (options_.max_file_size_ > 0 && stat_info_.st_size > options_.max_file_size_) {
        ret = common::OB_SIZE_OVERFLOW;
        eof_ = true;
      }
    }
    return ret;
  }

  int reset()
  {
    int ret = common::OB_SUCCESS;
    in_.clear();
    in_.seekg(0, std::ios::beg);
    peek_ = -1;
    eof_ = !in_.is_open();
    return ret;
  }

  int next(std::vector<std::string> &record)
  {
    int ret = common::OB_SUCCESS;
    record.clear();
    if (eof_) {
      ret = common::OB_ITER_END;
    } else if (options_.max_rows_ > 0 && row_count_ >= options_.max_rows_) {
      ret = common::OB_SIZE_OVERFLOW;
    } else if (stat_valid_ && OB_FAIL(check_unchanged_())) {
    } else {
      std::string field;
      bool in_quotes = false;
      bool any = false;
      while (true) {
        char ch = '\0';
        if (OB_FAIL(next_char_(ch))) {
          break;
        }
        any = true;
        if (in_quotes) {
          if (ch == options_.quote_) {
            char next = '\0';
            int next_ret = next_char_(next);
            if (common::OB_SUCCESS == next_ret && next == options_.quote_) {
              field.push_back(options_.quote_);
            } else {
              if (common::OB_SUCCESS == next_ret) {
                unget_char_(next);
              }
              in_quotes = false;
            }
          } else {
            field.push_back(ch);
          }
        } else if (ch == options_.quote_) {
          in_quotes = true;
        } else if (ch == options_.delimiter_) {
          record.push_back(field);
          field.clear();
        } else if (ch == '\n') {
          record.push_back(field);
          break;
        } else if (ch == '\r') {
          char next = '\0';
          int next_ret = next_char_(next);
          if (common::OB_SUCCESS == next_ret && next != '\n') {
            unget_char_(next);
          }
          record.push_back(field);
          break;
        } else {
          field.push_back(ch);
        }
      }
      if (OB_FAIL(ret)) {
        if (common::OB_ITER_END == ret && !any) {
          eof_ = true;
        }
      }
      if (OB_SUCC(ret)) {
        ++row_count_;
      }
    }
    return ret;
  }

private:
  int next_char_(char &ch)
  {
    int ret = common::OB_SUCCESS;
    if (peek_ >= 0) {
      ch = static_cast<char>(peek_);
      peek_ = -1;
    } else {
      int c = in_.get();
      if (c == std::char_traits<char>::eof()) {
        ret = common::OB_ITER_END;
      } else {
        ch = static_cast<char>(c);
      }
    }
    return ret;
  }

  void unget_char_(char ch)
  {
    peek_ = static_cast<unsigned char>(ch);
  }

private:
  int check_unchanged_()
  {
    int ret = common::OB_SUCCESS;
    struct stat current;
    if (0 != stat(path_.c_str(), &current)) {
      ret = common::OB_FILE_NOT_EXIST;
    } else if (current.st_size != stat_info_.st_size ||
               current.st_mtime != stat_info_.st_mtime) {
      ret = common::OB_IO_ERROR;
    }
    return ret;
  }

private:
  std::ifstream in_;
  ObFileSqlOptions options_;
  int peek_;
  bool eof_;
  bool stat_valid_;
  struct stat stat_info_;
  std::string path_;
  int64_t row_count_;
};

class ObFileJsonlReader
{
public:
  ObFileJsonlReader()
      : in_(),
        columns_(),
        eof_(true),
        stat_valid_(false),
        stat_info_(),
        path_(),
        row_count_(0)
  {
  }

  ~ObFileJsonlReader()
  {
    if (in_.is_open()) {
      in_.close();
    }
  }

  int open(const common::ObString &path)
  {
    int ret = common::OB_SUCCESS;
    columns_.clear();
    eof_ = false;
    stat_valid_ = false;
    row_count_ = 0;
    in_.clear();
    std::string path_str(path.ptr(), path.length());
    path_ = path_str;
    in_.open(path_str, std::ios::binary);
    if (!in_.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      eof_ = true;
    } else if (0 == stat(path_str.c_str(), &stat_info_)) {
      stat_valid_ = true;
      if (stat_info_.st_size > 1024LL * 1024LL * 1024LL) {
        ret = common::OB_SIZE_OVERFLOW;
        eof_ = true;
      }
    } else if (OB_FAIL(collect_columns_())) {
    } else if (OB_FAIL(reset())) {
    }
    return ret;
  }

  int reset()
  {
    int ret = common::OB_SUCCESS;
    in_.clear();
    in_.seekg(0, std::ios::beg);
    eof_ = !in_.is_open();
    return ret;
  }

  const std::vector<std::string> &columns() const
  {
    return columns_;
  }

  int next(std::vector<std::string> &values, std::vector<bool> &nulls)
  {
    int ret = common::OB_SUCCESS;
    values.assign(columns_.size(), "");
    nulls.assign(columns_.size(), true);
    if (eof_) {
      ret = common::OB_ITER_END;
    } else if (row_count_ >= 10000000) {
      ret = common::OB_SIZE_OVERFLOW;
    } else if (stat_valid_ && OB_FAIL(check_unchanged_())) {
    } else {
      std::string line;
      bool got_line = false;
      while (OB_SUCC(ret) && std::getline(in_, line)) {
        if (line.empty()) {
          continue;
        }
        got_line = true;
        rapidjson::Document doc;
        doc.Parse(line.c_str());
        if (doc.HasParseError() || !doc.IsObject()) {
          ret = common::OB_INVALID_DATA;
          break;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(columns_.size()); ++i) {
          rapidjson::Value::ConstMemberIterator it = doc.FindMember(columns_[i].c_str());
          if (it == doc.MemberEnd()) {
            nulls[i] = true;
            values[i] = "";
          } else if (it->value.IsNull()) {
            nulls[i] = true;
            values[i] = "";
          } else if (it->value.IsBool()) {
            nulls[i] = false;
            values[i] = it->value.GetBool() ? "true" : "false";
          } else if (it->value.IsInt64()) {
            nulls[i] = false;
            values[i] = std::to_string(it->value.GetInt64());
          } else if (it->value.IsDouble()) {
            nulls[i] = false;
            values[i] = std::to_string(it->value.GetDouble());
          } else if (it->value.IsString()) {
            nulls[i] = false;
            values[i] = it->value.GetString();
          } else {
            ret = common::OB_NOT_SUPPORTED;
          }
        }
        break;
      }
      if (OB_SUCC(ret) && !got_line) {
        ret = common::OB_ITER_END;
        eof_ = true;
      } else if (OB_SUCC(ret)) {
        ++row_count_;
      }
    }
    return ret;
  }

private:
  int collect_columns_()
  {
    int ret = common::OB_SUCCESS;
    std::string line;
    while (OB_SUCC(ret) && std::getline(in_, line)) {
      if (line.empty()) {
        continue;
      }
      rapidjson::Document doc;
      doc.Parse(line.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        ret = common::OB_INVALID_DATA;
        break;
      }
      for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin();
           it != doc.MemberEnd(); ++it) {
        const std::string key = it->name.GetString();
        if (it->value.IsObject() || it->value.IsArray()) {
          ret = common::OB_NOT_SUPPORTED;
          break;
        }
        if (std::find(columns_.begin(), columns_.end(), key) == columns_.end()) {
          columns_.push_back(key);
        }
      }
    }
    if (OB_SUCC(ret) && in_.bad()) {
      ret = common::OB_IO_ERROR;
    }
    return ret;
  }

private:
  int check_unchanged_()
  {
    int ret = common::OB_SUCCESS;
    struct stat current;
    if (0 != stat(path_.c_str(), &current)) {
      ret = common::OB_FILE_NOT_EXIST;
    } else if (current.st_size != stat_info_.st_size ||
               current.st_mtime != stat_info_.st_mtime) {
      ret = common::OB_IO_ERROR;
    }
    return ret;
  }

private:
  std::ifstream in_;
  std::vector<std::string> columns_;
  bool eof_;
  bool stat_valid_;
  struct stat stat_info_;
  std::string path_;
  int64_t row_count_;
};

inline bool snappy_read_varint_(const uint8_t *input,
                                size_t input_len,
                                size_t &pos,
                                uint64_t &value)
{
  value = 0;
  int shift = 0;
  while (pos < input_len && shift < 64) {
    uint8_t byte = input[pos++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      return true;
    }
    shift += 7;
  }
  return false;
}

inline int snappy_decompress(const char *input,
                             const size_t input_len,
                             std::vector<char> &output)
{
  int ret = common::OB_SUCCESS;
  output.clear();
  if (OB_ISNULL(input) || input_len == 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    const uint8_t *in = reinterpret_cast<const uint8_t *>(input);
    size_t pos = 0;
    uint64_t uncompressed_len = 0;
    if (!snappy_read_varint_(in, input_len, pos, uncompressed_len) ||
        uncompressed_len > static_cast<uint64_t>(INT64_MAX)) {
      ret = common::OB_INVALID_DATA;
    } else {
      output.reserve(static_cast<size_t>(uncompressed_len));
      while (OB_SUCC(ret) && pos < input_len &&
             output.size() < static_cast<size_t>(uncompressed_len)) {
        const uint8_t tag = in[pos++];
        const uint8_t type = tag & 0x03;
        if (0 == type) {
          uint64_t literal_len = (tag >> 2);
          if (literal_len < 60) {
            literal_len += 1;
          } else {
            const uint64_t extra_len = literal_len - 59;
            literal_len = 0;
            if (extra_len > 4 || pos + extra_len > input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              for (uint64_t i = 0; i < extra_len; ++i) {
                literal_len |= static_cast<uint64_t>(in[pos++]) << (8 * i);
              }
              literal_len += 1;
            }
          }
          if (OB_SUCC(ret) && (pos + literal_len > input_len ||
                               output.size() + literal_len > uncompressed_len)) {
            ret = common::OB_INVALID_DATA;
          } else if (OB_SUCC(ret)) {
            output.insert(output.end(), input + pos, input + pos + literal_len);
            pos += literal_len;
          }
        } else {
          uint64_t copy_len = 0;
          uint64_t offset = 0;
          if (1 == type) {
            if (pos >= input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              copy_len = ((tag >> 2) & 0x07) + 4;
              offset = ((tag & 0xe0) << 3) | in[pos++];
            }
          } else if (2 == type) {
            if (pos + 2 > input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              copy_len = (tag >> 2) + 1;
              offset = static_cast<uint64_t>(in[pos]) |
                       (static_cast<uint64_t>(in[pos + 1]) << 8);
              pos += 2;
            }
          } else {
            if (pos + 4 > input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              copy_len = (tag >> 2) + 1;
              offset = static_cast<uint64_t>(in[pos]) |
                       (static_cast<uint64_t>(in[pos + 1]) << 8) |
                       (static_cast<uint64_t>(in[pos + 2]) << 16) |
                       (static_cast<uint64_t>(in[pos + 3]) << 24);
              pos += 4;
            }
          }
          if (OB_SUCC(ret)) {
            if (0 == offset || offset > output.size() ||
                output.size() + copy_len > uncompressed_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              const size_t copy_start = output.size() - static_cast<size_t>(offset);
              for (uint64_t i = 0; i < copy_len; ++i) {
                output.push_back(output[copy_start + i]);
              }
            }
          }
        }
      }
      if (OB_SUCC(ret) && output.size() != static_cast<size_t>(uncompressed_len)) {
        ret = common::OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
