#pragma once

// File readers do not execute SQL. Text readers retain a schema and one bounded
// row; Parquet retains bounded pages and dictionaries for selected columns.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

enum class Type { Null, Bigint, Double, Boolean, Varchar, Date, Datetime };
struct Cell {
  std::string text;
  Type type = Type::Null;
};
struct Column {
  std::string name;
  Type type = Type::Null;
};
struct Options {
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_text;
  int64_t sample_size = -1;
};
struct Error {
  uint64_t row = 0;
  uint64_t column = 0;
  std::string message;
  std::string diagnostic() const {
    return message + " (row " + std::to_string(row) + ", column " + std::to_string(column) + ")";
  }
};

Type infer_type(const std::string &text);
Type widen(Type left, Type right);
bool valid_utf8(const std::string &text);
std::string json_string(const std::string &text);

// Fail closed when the allowed directory is empty or NULL. Validation is done
// against the opened handle, not just against the spelling of the input path.
class File {
public:
  File() = default;
  ~File();
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  bool open(const std::string &path, const std::string &allowed, Error &error);
  bool unchanged(Error &error) const;
  bool rewind(Error &error);
  FILE *stream() const { return stream_; }
  const std::string &identity() const { return identity_; }

private:
  bool snapshot(std::string &result) const;
  FILE *stream_ = nullptr;
  std::string path_;
  std::string identity_;
};

class ParquetReader;
// Writes to a private temporary file and publishes with no-replace semantics.
// Destruction before commit removes the temporary file.
class CsvWriter {
public:
  explicit CsvWriter(const Options &options);
  ~CsvWriter();
  bool open(const std::string &path, const std::string &allowed, Error &error);
  bool write(const std::vector<Cell> &row, Error &error);
  bool commit(Error &error);

private:
  struct State;
  std::unique_ptr<State> state_;
};
class Reader {
public:
  enum class Format { Csv, Jsonl, Glob, Parquet };
  static constexpr size_t MAX_ROW_BYTES = 8 * 1024 * 1024;
  static constexpr size_t MAX_COLUMNS = 1024;
  static constexpr size_t MAX_SCHEMA_BYTES = 1024 * 1024;
  Reader(Format format, const Options &options);
  ~Reader();
  bool open(const std::string &path, const std::string &allowed);
  bool infer();
  void set_cancel_check(std::function<bool()> check) { cancelled_ = std::move(check); }
  bool rescan();
  // false means EOF or error; error().message distinguishes the two.
  bool next(std::vector<Cell> &row);
  void project(const std::vector<size_t> &indices);
  bool next_batch(size_t max_rows, std::vector<std::vector<Cell>> &rows);
  const std::vector<Column> &columns() const { return columns_; }
  const Error &error() const { return error_; }
  const std::string &identity() const {
    return format_ == Format::Glob ? discovery_identity_ : file_.identity();
  }

private:
  bool csv(std::vector<Cell> &row);
  bool json(std::vector<Cell> &row, std::vector<std::string> &names);
  bool raw(std::vector<Cell> &row, std::vector<std::string> &names);
  bool fail(const char *message, size_t column = 0);
  std::function<bool()> cancelled_;
  File file_;
  Format format_;
  Options options_;
  Error error_;
  uint64_t row_number_ = 0;
  bool inferred_ = false;
  std::vector<Column> columns_;
  std::vector<std::string> discovered_;
  std::string discovery_identity_;
  size_t discovery_position_ = 0;
  std::unique_ptr<ParquetReader> parquet_;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
