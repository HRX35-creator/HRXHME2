/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#pragma once

#include <cstdio>
#include <sys/stat.h>
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "common/object/ob_object.h"

namespace oceanbase { namespace sql {
class FileSqlGlob;

enum class FileSqlFormat { CSV, JSONL, PARQUET, GLOB };
enum class FileSqlType { NIL, I64, F64, BOOL, TEXT, DATE, DATETIME };

struct FileSqlOptions {
  FileSqlOptions() : format_(FileSqlFormat::CSV), header_(true), delimiter_(','),
                     quote_('"'), escape_('"') {}
  common::ObString path_;
  FileSqlFormat format_;
  bool header_;
  char delimiter_;
  char quote_;
  char escape_;
  common::ObString null_text_;
};

struct FileSqlColumn {
  common::ObString name_;
  FileSqlType type_ = FileSqlType::NIL;
  bool nullable_ = false;
  int64_t max_length_ = 0;
};

// A reader keeps only one record and its schema. No file data is retained after
// next_row(); callers must copy values before advancing or rescanning.
class FileSqlReader {
public:
  FileSqlReader();
  ~FileSqlReader();
  int open(const FileSqlOptions &options, const common::ObString &secure_root);
  int infer_schema();
  int make_descriptor(common::ObIAllocator &allocator, common::ObString &descriptor) const;
  int open_descriptor(const common::ObString &descriptor, const common::ObString &secure_root);
  int next_row(common::ObIArray<common::ObObj> &row);
  int project(const common::ObIArray<int64_t> &indices);
  int rescan();
  void close();
  const common::ObIArray<FileSqlColumn> &columns() const { return columns_; }
  static common::ObObjType object_type(FileSqlType type);
  static const char *type_name(FileSqlType type);
  static bool valid_utf8(const common::ObString &value);
  static int check_path(const common::ObString &path, const common::ObString &root,
                        common::ObIAllocator &allocator, common::ObString &canonical);
private:
  struct Cell {
    common::ObString name_;
    common::ObString value_;
    FileSqlType type_ = FileSqlType::NIL;
    bool null_ = false;
  };
  int read_record();
  int read_csv();
  int read_jsonl();
  int check_unchanged() const;
  int add_column(const common::ObString &name, bool deduplicate, bool nullable);
  int convert(const Cell &cell, const FileSqlColumn &column, common::ObObj &value);
  int error(const char *message) const;
  static FileSqlType infer_type(const common::ObString &value);
  static FileSqlType widen(FileSqlType left, FileSqlType right);
  static bool parse_integer(const common::ObString &value, int64_t &number);
  static bool parse_datetime(const common::ObString &value, bool date_only, int64_t &number);
  FileSqlOptions options_;
  common::ObArenaAllocator meta_allocator_;
  common::ObArenaAllocator row_allocator_;
  common::ObSEArray<FileSqlColumn, 16> columns_;
  common::ObSEArray<Cell, 16> cells_;
  common::ObString canonical_path_;
  FILE *file_;
  struct stat snapshot_;
  int64_t line_;
  int64_t record_line_;
  int64_t data_offset_;
  bool schema_ready_;
  void *parquet_;
  FileSqlGlob *glob_;
  static constexpr int64_t MAX_COLUMNS = 1024;
  static constexpr int64_t MAX_RECORD_BYTES = 16 * 1024 * 1024;
  static constexpr int64_t MAX_CELL_BYTES = 4 * 1024 * 1024;
  DISALLOW_COPY_AND_ASSIGN(FileSqlReader);
};

} } // namespace oceanbase::sql
