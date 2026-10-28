/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "sql/file_sql/ob_file_sql_scan_utils.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{

constexpr size_t MAX_RECORD_BYTES = 64ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_FIELDS = 4096;

std::string trim(std::string_view value)
{
  size_t first = 0;
  size_t last = value.size();
  while (first < last && std::isspace(static_cast<unsigned char>(value[first]))) { ++first; }
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) { --last; }
  return std::string(value.substr(first, last - first));
}

bool is_integer(std::string_view value)
{
  if (value.empty()) { return false; }
  size_t pos = value[0] == '-' || value[0] == '+' ? 1 : 0;
  if (pos == value.size()) { return false; }
  for (; pos < value.size(); ++pos) {
    if (!std::isdigit(static_cast<unsigned char>(value[pos]))) { return false; }
  }
  int64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  return result.ec == std::errc() && result.ptr == value.data() + value.size();
}

bool is_double(std::string_view value)
{
  if (value.empty() || is_integer(value)) { return false; }
  std::string copy(value);
  char *end = nullptr;
  const double parsed = std::strtod(copy.c_str(), &end);
  return end != copy.c_str() && *end == '\0' && std::isfinite(parsed);
}

bool is_date(std::string_view value)
{
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') { return false; }
  for (size_t i = 0; i < value.size(); ++i) {
    if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(value[i]))) { return false; }
  }
  const int year = std::atoi(std::string(value.substr(0, 4)).c_str());
  const int month = std::atoi(std::string(value.substr(5, 2)).c_str());
  const int day = std::atoi(std::string(value.substr(8, 2)).c_str());
  static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  const int max_day = month == 2 && leap ? 29 : (month >= 1 && month <= 12 ? days[month] : 0);
  return year >= 1 && month >= 1 && month <= 12 && day >= 1 && day <= max_day;
}

bool is_datetime(std::string_view value)
{
  if (value.size() < 19 || !is_date(value.substr(0, 10))) { return false; }
  return (value[10] == ' ' || value[10] == 'T') && value[13] == ':' && value[16] == ':' &&
         std::isdigit(static_cast<unsigned char>(value[11])) &&
         std::isdigit(static_cast<unsigned char>(value[12])) &&
         std::isdigit(static_cast<unsigned char>(value[14])) &&
         std::isdigit(static_cast<unsigned char>(value[15])) &&
         std::isdigit(static_cast<unsigned char>(value[17])) &&
         std::isdigit(static_cast<unsigned char>(value[18]));
}

FileSqlType type_of(std::string_view raw)
{
  const std::string value = trim(raw);
  if (value.empty()) { return FileSqlType::NULL_TYPE; }
  if (value == "true" || value == "false" || value == "TRUE" || value == "FALSE") {
    return FileSqlType::BOOLEAN;
  }
  if (is_integer(value)) { return FileSqlType::BIGINT; }
  if (is_double(value)) { return FileSqlType::DOUBLE; }
  if (is_datetime(value)) { return FileSqlType::DATETIME; }
  if (is_date(value)) { return FileSqlType::DATE; }
  return FileSqlType::VARCHAR;
}

void set_error(FileSqlError &error, int64_t line, int64_t column, const char *message)
{
  error.line = line;
  error.column = column;
  error.message = message;
}

void skip_space(std::string_view text, size_t &pos)
{
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) { ++pos; }
}

bool parse_json_string(std::string_view text, size_t &pos, std::string &value)
{
  if (pos >= text.size() || text[pos] != '"') { return false; }
  ++pos;
  value.clear();
  while (pos < text.size()) {
    const char c = text[pos++];
    if (c == '"') { return true; }
    if (c == '\\') {
      if (pos >= text.size()) { return false; }
      const char escaped = text[pos++];
      switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return false;
      }
    } else if (static_cast<unsigned char>(c) < 0x20) {
      return false;
    } else {
      value.push_back(c);
    }
  }
  return false;
}

} // namespace

bool is_path_allowed(const std::filesystem::path &secure_root,
                     const std::filesystem::path &candidate,
                     std::filesystem::path *resolved)
{
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(secure_root, error);
  if (error) { return false; }
  const auto path = std::filesystem::weakly_canonical(candidate, error);
  if (error || path.empty()) { return false; }
  auto root_it = root.begin();
  auto path_it = path.begin();
  for (; root_it != root.end() && path_it != path.end(); ++root_it, ++path_it) {
    if (*root_it != *path_it) { return false; }
  }
  if (root_it != root.end()) { return false; }
  if (resolved != nullptr) { *resolved = path; }
  return true;
}

bool infer_value_type(std::string_view value, FileSqlType &type)
{
  type = type_of(value);
  return true;
}

FileSqlType widen_type(FileSqlType left, FileSqlType right)
{
  if (left == FileSqlType::NULL_TYPE) { return right; }
  if (right == FileSqlType::NULL_TYPE || left == right) { return left; }
  if ((left == FileSqlType::BIGINT && right == FileSqlType::DOUBLE) ||
      (left == FileSqlType::DOUBLE && right == FileSqlType::BIGINT)) {
    return FileSqlType::DOUBLE;
  }
  return FileSqlType::VARCHAR;
}

bool CsvReader::open(const std::filesystem::path &path, bool header, char delimiter,
                     std::string_view null_string, FileSqlError &error)
{
  close();
  path_ = path;
  header_ = header;
  delimiter_ = delimiter;
  null_string_ = std::string(null_string);
  input_.open(path_, std::ios::binary);
  if (!input_.is_open()) {
    set_error(error, 0, 0, "cannot open CSV file");
    return false;
  }
  std::error_code stat_error;
  file_size_ = std::filesystem::file_size(path_, stat_error);
  file_time_ = std::filesystem::last_write_time(path_, stat_error);
  if (stat_error) { set_error(error, 0, 0, "cannot stat CSV file"); return false; }
  line_ = 0;
  if (header_) {
    std::vector<std::string> fields;
    if (!read_record(fields, error) || fields.empty()) { return false; }
    header_names_ = std::move(fields);
    for (size_t i = 0; i < header_names_.size(); ++i) {
      const std::string original = header_names_[i];
      int64_t occurrence = 1;
      for (size_t j = 0; j < i; ++j) {
        if (header_names_[j] == original ||
            (header_names_[j].size() > original.size() + 2 &&
             header_names_[j].compare(0, original.size(), original) == 0 &&
             header_names_[j].compare(original.size(), 2, "__") == 0)) {
          ++occurrence;
        }
      }
      if (occurrence > 1) { header_names_[i] = original + "__" + std::to_string(occurrence); }
    }
  }
  return true;
}

bool CsvReader::read_record(std::vector<std::string> &fields, FileSqlError &error)
{
  if (!check_unchanged(error)) { return false; }
  fields.clear();
  std::string record;
  bool quoted = false;
  bool read_any = false;
  char c = 0;
  while (input_.get(c)) {
    read_any = true;
    if (c == '"') {
      quoted = !quoted;
    } else if ((c == '\n' || c == '\r') && !quoted) {
      if (c == '\r' && input_.peek() == '\n') { input_.get(); }
      ++line_;
      return parse_record(record, fields, error);
    }
    record.push_back(c);
    if (record.size() > MAX_RECORD_BYTES) {
      set_error(error, line_ + 1, 0, "CSV record exceeds the resource limit");
      return false;
    }
  }
  if (quoted) { set_error(error, line_ + 1, 0, "unterminated quoted field"); return false; }
  if (read_any || !record.empty()) {
    ++line_;
    return parse_record(record, fields, error);
  }
  return false;
}

bool CsvReader::parse_record(std::string_view record, std::vector<std::string> &fields,
                             FileSqlError &error) const
{
  std::string field;
  bool quoted = false;
  for (size_t i = 0; i < record.size(); ++i) {
    const char c = record[i];
    if (c == '"') {
      if (quoted && i + 1 < record.size() && record[i + 1] == '"') {
        field.push_back('"');
        ++i;
      } else {
        quoted = !quoted;
      }
    } else if (c == delimiter_ && !quoted) {
      fields.push_back(std::move(field));
      field.clear();
    } else {
      field.push_back(c);
    }
  }
  if (quoted) { set_error(error, line_, 0, "unterminated quoted field"); return false; }
  fields.push_back(std::move(field));
  if (fields.size() > MAX_FIELDS) {
    set_error(error, line_, fields.size(), "CSV record has too many fields");
    return false;
  }
  return true;
}

bool CsvReader::check_unchanged(FileSqlError &error) const
{
  std::error_code stat_error;
  const auto size = std::filesystem::file_size(path_, stat_error);
  const auto time = std::filesystem::last_write_time(path_, stat_error);
  if (stat_error || size != file_size_ || time != file_time_) {
    set_error(error, line_ + 1, 0, "file changed while it was being scanned");
    return false;
  }
  return true;
}

bool CsvReader::rewind(FileSqlError &error)
{
  input_.clear();
  input_.seekg(0);
  line_ = 0;
  if (!input_) { set_error(error, 0, 0, "cannot rewind CSV file"); return false; }
  if (header_) {
    std::vector<std::string> ignored;
    if (!read_record(ignored, error)) { return false; }
  }
  return true;
}

bool CsvReader::infer_schema(std::vector<FileSqlColumn> &columns, FileSqlError &error)
{
  columns.clear();
  if (header_names_.empty() && header_) { set_error(error, line_, 0, "CSV header is empty"); return false; }
  std::vector<std::string> fields;
  if (!header_ && !read_record(fields, error)) { return false; }
  if (!header_) {
    header_names_.resize(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) { header_names_[i] = "column" + std::to_string(i + 1); }
    rewind(error);
  }
  std::vector<FileSqlType> types(header_names_.size(), FileSqlType::NULL_TYPE);
  while (read_record(fields, error)) {
    if (fields.size() != types.size()) {
      set_error(error, line_, fields.size(), "CSV row has the wrong number of fields");
      return false;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i] != null_string_) { types[i] = widen_type(types[i], type_of(fields[i])); }
    }
  }
  if (input_.bad()) { set_error(error, line_, 0, "CSV read failed"); return false; }
  for (size_t i = 0; i < types.size(); ++i) { columns.push_back({header_names_[i], types[i]}); }
  schema_ = columns;
  return rewind(error);
}

bool CsvReader::next(std::vector<FileSqlValue> &row, FileSqlError &error)
{
  std::vector<std::string> fields;
  if (!read_record(fields, error)) { return false; }
  if (!schema_.empty() && fields.size() != schema_.size()) {
    set_error(error, line_, fields.size(), "CSV row has the wrong number of fields");
    return false;
  }
  row.clear();
  for (const auto &field : fields) { row.push_back({field.empty() || field == null_string_, field}); }
  return true;
}

void CsvReader::close()
{
  if (input_.is_open()) { input_.close(); }
  header_names_.clear();
  schema_.clear();
  line_ = 0;
}

bool JsonlReader::open(const std::filesystem::path &path, FileSqlError &error)
{
  close();
  path_ = path;
  input_.open(path_, std::ios::binary);
  if (!input_.is_open()) { set_error(error, 0, 0, "cannot open JSONL file"); return false; }
  std::error_code stat_error;
  file_size_ = std::filesystem::file_size(path_, stat_error);
  file_time_ = std::filesystem::last_write_time(path_, stat_error);
  if (stat_error) { set_error(error, 0, 0, "cannot stat JSONL file"); return false; }
  line_ = 0;
  return true;
}

bool JsonlReader::parse_object(std::string_view text,
                               std::vector<std::pair<std::string, FileSqlValue>> &object,
                               FileSqlError &error) const
{
  object.clear();
  size_t pos = 0;
  skip_space(text, pos);
  if (pos >= text.size() || text[pos++] != '{') { set_error(error, line_, pos, "JSONL line must be an object"); return false; }
  skip_space(text, pos);
  if (pos < text.size() && text[pos] == '}') { return true; }
  while (pos < text.size()) {
    std::string key;
    skip_space(text, pos);
    if (!parse_json_string(text, pos, key)) { set_error(error, line_, pos, "invalid JSON object key"); return false; }
    skip_space(text, pos);
    if (pos >= text.size() || text[pos++] != ':') { set_error(error, line_, pos, "missing JSON colon"); return false; }
    skip_space(text, pos);
    FileSqlValue value;
    if (pos < text.size() && text[pos] == '"') {
      value.is_null = false;
      if (!parse_json_string(text, pos, value.text)) { set_error(error, line_, pos, "invalid JSON string"); return false; }
    } else {
      const size_t begin = pos;
      while (pos < text.size() && text[pos] != ',' && text[pos] != '}') { ++pos; }
      const std::string token = trim(text.substr(begin, pos - begin));
      if (token == "null") { value.is_null = true; }
      else if (token == "true" || token == "false" || is_integer(token) || is_double(token)) {
        value.is_null = false; value.text = token;
      } else { set_error(error, line_, begin, "nested JSON objects and arrays are not supported"); return false; }
    }
    object.emplace_back(std::move(key), std::move(value));
    skip_space(text, pos);
    if (pos >= text.size() || text[pos] == '}') { if (pos < text.size()) { ++pos; } break; }
    if (text[pos++] != ',') { set_error(error, line_, pos, "missing JSON comma"); return false; }
  }
  skip_space(text, pos);
  if (pos != text.size()) { set_error(error, line_, pos, "trailing JSON data"); return false; }
  return true;
}

bool JsonlReader::rewind(FileSqlError &error)
{
  input_.clear(); input_.seekg(0); line_ = 0;
  if (!input_) { set_error(error, 0, 0, "cannot rewind JSONL file"); return false; }
  return true;
}

bool JsonlReader::infer_schema(std::vector<FileSqlColumn> &columns, FileSqlError &error)
{
  columns.clear();
  std::vector<std::string> names;
  std::vector<FileSqlType> types;
  std::string line;
  while (std::getline(input_, line)) {
    if (!check_unchanged(error)) { return false; }
    if (line.size() > MAX_RECORD_BYTES) {
      set_error(error, line_, 0, "JSONL record exceeds the resource limit");
      return false;
    }
    ++line_;
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (trim(line).empty()) { continue; }
    std::vector<std::pair<std::string, FileSqlValue>> object;
    if (!parse_object(line, object, error)) { return false; }
    for (const auto &entry : object) {
      auto it = std::find(names.begin(), names.end(), entry.first);
      size_t index = static_cast<size_t>(it - names.begin());
      if (it == names.end()) {
        names.push_back(entry.first);
        types.push_back(FileSqlType::NULL_TYPE);
        index = names.size() - 1;
      }
      if (!entry.second.is_null) { types[index] = widen_type(types[index], type_of(entry.second.text)); }
    }
  }
  if (input_.bad()) { set_error(error, line_, 0, "JSONL read failed"); return false; }
  for (size_t i = 0; i < names.size(); ++i) { columns.push_back({names[i], types[i]}); }
  schema_ = columns;
  return rewind(error);
}

bool JsonlReader::next(std::vector<FileSqlValue> &row, FileSqlError &error)
{
  std::string line;
  while (std::getline(input_, line)) {
    if (!check_unchanged(error)) { return false; }
    if (line.size() > MAX_RECORD_BYTES) {
      set_error(error, line_, 0, "JSONL record exceeds the resource limit");
      return false;
    }
    ++line_;
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (trim(line).empty()) { continue; }
    std::vector<std::pair<std::string, FileSqlValue>> object;
    if (!parse_object(line, object, error)) { return false; }
    row.assign(schema_.size(), FileSqlValue{});
    for (const auto &entry : object) {
      for (size_t i = 0; i < schema_.size(); ++i) {
        if (schema_[i].name == entry.first) { row[i] = entry.second; break; }
      }
    }
    return true;
  }
  return false;
}

bool JsonlReader::check_unchanged(FileSqlError &error) const
{
  std::error_code stat_error;
  const auto size = std::filesystem::file_size(path_, stat_error);
  const auto time = std::filesystem::last_write_time(path_, stat_error);
  if (stat_error || size != file_size_ || time != file_time_) {
    set_error(error, line_ + 1, 0, "file changed while it was being scanned");
    return false;
  }
  return true;
}

void JsonlReader::close()
{
  if (input_.is_open()) { input_.close(); }
  schema_.clear();
  line_ = 0;
  file_size_ = 0;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
