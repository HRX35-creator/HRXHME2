/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG

#include "sql/file_sql/ob_file_table_function.h"
#include "sql/file_sql/ob_file_sql_scan_utils.h"
#include "lib/ob_errno.h"

#include <cstdlib>

namespace oceanbase { namespace sql {
namespace {
using namespace common;
using file_sql::CsvReader;
using file_sql::FileSqlColumn;
using file_sql::FileSqlError;
using file_sql::FileSqlType;
using file_sql::FileSqlValue;
using file_sql::JsonlReader;

class FileTableScan final : public ITableFunctionScan
{
public:
  explicit FileTableScan(const TableFunctionScanContext &context)
    : context_(context), columns_(), values_(), cells_(), path_(), opened_(false) {}

  int open() override
  {
    int ret = OB_SUCCESS;
    if (context_.arguments_.count() < 1 || context_.arguments_.at(0).is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const ObString path = context_.arguments_.at(0).get_string();
      path_.assign(path.ptr(), path.length());
      FileSqlError error;
      if (context_.descriptor_.id_ == READ_CSV_FUNCTION_ID) {
        if (!csv_.open(path_, true, ',', "", error) || !csv_.infer_schema(columns_, error)) { ret = OB_ERR_UNEXPECTED; }
      } else if (!jsonl_.open(path_, error) || !jsonl_.infer_schema(columns_, error)) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (OB_SUCC(ret) && columns_.size() != static_cast<size_t>(context_.column_ids_.count())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_SUCC(ret)) {
        cells_.resize(columns_.size());
        opened_ = true;
      }
    }
    return ret;
  }

  int rescan(const common::ObIArray<common::ObObj> &arguments) override
  { UNUSED(arguments); close(); return open(); }

  int get_next_row(common::ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    FileSqlError error;
    if (!opened_) { ret = OB_NOT_INIT; }
    if (OB_SUCC(ret)) {
      bool has_row = context_.descriptor_.id_ == READ_CSV_FUNCTION_ID
          ? csv_.next(values_, error) : jsonl_.next(values_, error);
      if (!has_row) { ret = error.message.empty() ? OB_ITER_END : OB_ERR_UNEXPECTED; }
      else if (values_.size() != cells_.size()) { ret = OB_ERR_UNEXPECTED; }
      else {
        for (size_t i = 0; i < values_.size(); ++i) {
          const FileSqlValue &value = values_[i];
          if (value.is_null) { cells_[i].set_null(); }
          else if (columns_[i].type == FileSqlType::BIGINT) {
            cells_[i].set_int(std::strtoll(value.text.c_str(), nullptr, 10));
          } else if (columns_[i].type == FileSqlType::DOUBLE) {
            cells_[i].set_double(std::strtod(value.text.c_str(), nullptr));
          } else if (columns_[i].type == FileSqlType::BOOLEAN) {
            cells_[i].set_bool(value.text == "true" || value.text == "TRUE");
          } else {
            cells_[i].set_varchar(ObString(value.text.size(), value.text.data()));
          }
        }
        row_.cells_ = cells_.data();
        row_.count_ = static_cast<int64_t>(cells_.size());
        row = &row_;
      }
    }
    return ret;
  }

  int close() override
  {
    csv_.close(); jsonl_.close(); columns_.clear(); values_.clear(); cells_.clear(); opened_ = false;
    return OB_SUCCESS;
  }

private:
  const TableFunctionScanContext &context_;
  CsvReader csv_;
  JsonlReader jsonl_;
  std::vector<FileSqlColumn> columns_;
  std::vector<FileSqlValue> values_;
  std::vector<common::ObObj> cells_;
  std::filesystem::path path_;
  common::ObNewRow row_;
  bool opened_;
};
}

int create_file_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  void *memory = context.allocator_.alloc(sizeof(FileTableScan));
  if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  scan = new (memory) FileTableScan(context);
  return OB_SUCCESS;
}
}}
