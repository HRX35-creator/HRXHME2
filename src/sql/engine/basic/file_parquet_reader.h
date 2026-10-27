#pragma once
#include "file_scan_utils.h"

namespace oceanbase {
namespace sql {
namespace file_sql {
// Format decoder only: SQL evaluation is performed by the native SQL operators.
class ParquetReader {
public:
  ParquetReader();
  ~ParquetReader();
  bool infer(File &file, std::vector<Column> &columns, Error &error);
  bool next(std::vector<Cell> &row, Error &error);
  bool rescan(Error &error);
  void project(const std::vector<size_t> &indices);

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
