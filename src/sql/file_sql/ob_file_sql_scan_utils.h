/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

enum class FileSqlType : uint8_t
{
  NULL_TYPE,
  BIGINT,
  DOUBLE,
  BOOLEAN,
  VARCHAR,
  DATE,
  DATETIME
};

struct FileSqlValue
{
  bool is_null = true;
  std::string text;
};

struct FileSqlColumn
{
  std::string name;
  FileSqlType type = FileSqlType::NULL_TYPE;
};

struct FileSqlError
{
  int64_t line = 0;
  int64_t column = 0;
  std::string message;
};

// All file access goes through this check. Both paths are canonicalized so a
// lexical ".." or a symlink cannot escape the configured directory.
bool is_path_allowed(const std::filesystem::path &secure_root,
                     const std::filesystem::path &candidate,
                     std::filesystem::path *resolved = nullptr);

bool infer_value_type(std::string_view value, FileSqlType &type);
FileSqlType widen_type(FileSqlType left, FileSqlType right);

class CsvReader
{
public:
  CsvReader() = default;
  ~CsvReader() = default;

  bool open(const std::filesystem::path &path, bool header, char delimiter,
            std::string_view null_string, FileSqlError &error);
  bool infer_schema(std::vector<FileSqlColumn> &columns, FileSqlError &error);
  bool next(std::vector<FileSqlValue> &row, FileSqlError &error);
  void close();

private:
  bool check_unchanged(FileSqlError &error) const;
  bool read_record(std::vector<std::string> &fields, FileSqlError &error);
  bool parse_record(std::string_view record, std::vector<std::string> &fields,
                    FileSqlError &error) const;
  bool rewind(FileSqlError &error);

  std::filesystem::path path_;
  std::ifstream input_;
  bool header_ = true;
  char delimiter_ = ',';
  std::string null_string_;
  std::vector<std::string> header_names_;
  std::vector<FileSqlColumn> schema_;
  int64_t line_ = 0;
  uintmax_t file_size_ = 0;
  std::filesystem::file_time_type file_time_{};
};

class JsonlReader
{
public:
  JsonlReader() = default;
  ~JsonlReader() = default;

  bool open(const std::filesystem::path &path, FileSqlError &error);
  bool infer_schema(std::vector<FileSqlColumn> &columns, FileSqlError &error);
  bool next(std::vector<FileSqlValue> &row, FileSqlError &error);
  void close();

private:
  bool check_unchanged(FileSqlError &error) const;
  bool parse_object(std::string_view text,
                    std::vector<std::pair<std::string, FileSqlValue>> &object,
                    FileSqlError &error) const;
  bool rewind(FileSqlError &error);

  std::filesystem::path path_;
  std::ifstream input_;
  std::vector<FileSqlColumn> schema_;
  int64_t line_ = 0;
  uintmax_t file_size_ = 0;
  std::filesystem::file_time_type file_time_{};
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
