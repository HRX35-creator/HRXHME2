#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
        null_text_()
  {
  }
  bool header_;
  char delimiter_;
  char quote_;
  int64_t sample_size_;
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
    } else {
      ret = common::OB_NOT_SUPPORTED;
      error_msg = common::ObString("parquet/glob schema inference is not implemented yet");
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
        eof_(true)
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
    in_.clear();
    in_.open(std::string(path.ptr(), path.length()), std::ios::binary);
    if (!in_.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      eof_ = true;
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
  std::ifstream in_;
  ObFileSqlOptions options_;
  int peek_;
  bool eof_;
};

class ObFileJsonlReader
{
public:
  ObFileJsonlReader()
      : in_(),
        columns_(),
        eof_(true)
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
    in_.clear();
    in_.open(std::string(path.ptr(), path.length()), std::ios::binary);
    if (!in_.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      eof_ = true;
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
    } else {
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
      if (OB_SUCC(ret) && in_.eof() && values.empty()) {
        ret = common::OB_ITER_END;
        eof_ = true;
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
  std::ifstream in_;
  std::vector<std::string> columns_;
  bool eof_;
};

} // namespace sql
} // namespace oceanbase
