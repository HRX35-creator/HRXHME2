/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_utils.h"
#if !defined(_WIN32)
#include "sql/engine/basic/ob_file_sql_glob.h"
#include "file_sql_bridge.h"
#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include "lib/string/ob_string_buffer.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "share/ob_errno.h"
#include "lib/worker.h"

namespace oceanbase { namespace sql {
using namespace common;

namespace {
// Walk beneath an already opened directory, rejecting symlink substitutions in
// every component. realpath alone does not protect against a concurrent rename.
int file_sql_open_beneath(int directory, const char *relative, bool final_directory)
{
  int current = ::dup(directory);
  const char *part = relative;
  while (current >= 0 && *part != '\0') {
    const char *end = strchr(part, '/');
    int64_t length = end == nullptr ? strlen(part) : end - part;
    char name[NAME_MAX + 1];
    if (length <= 0 || length > NAME_MAX || (length == 1 && part[0] == '.')
        || (length == 2 && part[0] == '.' && part[1] == '.')) {
      ::close(current);
      current = -1;
      break;
    }
    memcpy(name, part, length);
    name[length] = '\0';
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (end != nullptr || final_directory) { flags |= O_DIRECTORY; }
    int next = ::openat(current, name, flags);
    ::close(current);
    current = next;
    if (end == nullptr) { break; }
    part = end + 1;
  }
  return current;
}

bool file_sql_same_file(const struct stat &left, const struct stat &right)
{
  bool same = left.st_dev == right.st_dev && left.st_ino == right.st_ino
              && left.st_size == right.st_size;
#if defined(__APPLE__)
  same = same && left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec
              && left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec
              && left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec
              && left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
  same = same && left.st_mtim.tv_sec == right.st_mtim.tv_sec
              && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec
              && left.st_ctim.tv_sec == right.st_ctim.tv_sec
              && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
  return same;
}

bool file_sql_parse_double(const ObString &text, double &number)
{
  bool valid = !text.empty() && text.length() < 128;
  int64_t i = 0;
  if (valid && (text[0] == '+' || text[0] == '-')) { ++i; }
  int64_t digits = 0;
  while (valid && i < text.length() && text[i] >= '0' && text[i] <= '9') { ++i; ++digits; }
  if (valid && i < text.length() && text[i] == '.') {
    ++i;
    while (i < text.length() && text[i] >= '0' && text[i] <= '9') { ++i; ++digits; }
  }
  valid = valid && digits > 0;
  if (valid && i < text.length() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.length() && (text[i] == '+' || text[i] == '-')) { ++i; }
    int64_t exponent_digits = 0;
    while (i < text.length() && text[i] >= '0' && text[i] <= '9') { ++i; ++exponent_digits; }
    valid = exponent_digits > 0;
  }
  valid = valid && i == text.length();
  if (valid) {
    char buffer[128];
    memcpy(buffer, text.ptr(), text.length());
    buffer[text.length()] = '\0';
    char *end = nullptr;
    errno = 0;
    number = strtod(buffer, &end);
    valid = errno != ERANGE && end == buffer + text.length() && std::isfinite(number);
  }
  return valid;
}
} // namespace

FileSqlReader::FileSqlReader()
    : meta_allocator_("FileSqlMeta"), row_allocator_("FileSqlRow"), file_(nullptr),
      line_(0), record_line_(1), data_offset_(0), schema_ready_(false), parquet_(nullptr), glob_(nullptr)
{
  memset(&snapshot_, 0, sizeof(snapshot_));
}
FileSqlReader::~FileSqlReader() { close(); }

int FileSqlReader::error(const char *message) const
{
  int ret = OB_INVALID_DATA;
  char detail[512];
  snprintf(detail, sizeof(detail), "File SQL at line %ld: %s", record_line_, message);
  FORWARD_USER_ERROR(ret, detail);
  return ret;
}

int FileSqlReader::check_path(const ObString &path, const ObString &root,
                             ObIAllocator &allocator, ObString &canonical)
{
  int ret = OB_SUCCESS;
  char path_buffer[PATH_MAX + 1];
  char root_buffer[PATH_MAX + 1];
  char resolved[PATH_MAX + 1];
  char allowed[PATH_MAX + 1];
  if (path.empty() || root.empty() || root.case_compare("NULL") == 0
      || path.length() > PATH_MAX || root.length() > PATH_MAX
      || memchr(path.ptr(), '\0', path.length()) != nullptr
      || memchr(root.ptr(), '\0', root.length()) != nullptr) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else {
    memcpy(path_buffer, path.ptr(), path.length());
    path_buffer[path.length()] = '\0';
    memcpy(root_buffer, root.ptr(), root.length());
    root_buffer[root.length()] = '\0';
    struct stat root_stat;
    if (::realpath(root_buffer, allowed) == nullptr || ::stat(allowed, &root_stat) != 0
        || !S_ISDIR(root_stat.st_mode)) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else if (::realpath(path_buffer, resolved) == nullptr) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      int64_t length = strlen(allowed);
      if (strncmp(resolved, allowed, length) != 0
          || (length != 1 && resolved[length] != '/' && resolved[length] != '\0')) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else if (OB_FAIL(ob_write_string(allocator, ObString(resolved), canonical, true))) {
      }
    }
  }
  if (ret == OB_ERR_NO_PRIVILEGE) {
    LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "File SQL path is outside secure_file_priv");
  }
  return ret;
}

int FileSqlReader::open(const FileSqlOptions &options, const ObString &secure_root)
{
  int ret = OB_SUCCESS;
  close();
  options_ = options;
  if (options_.format_ == FileSqlFormat::GLOB) {
    if ((glob_ = OB_NEWx(FileSqlGlob, &meta_allocator_)) == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(glob_->open(options.path_, secure_root))) {
    } else if (OB_FAIL(ob_write_string(meta_allocator_, glob_->pattern(), canonical_path_, true))) {
    } else { snapshot_ = glob_->snapshot(); }
    return ret;
  }
  if (options.delimiter_ == '\0' || options.quote_ == '\0' || options.escape_ == '\0'
      || options.delimiter_ < 0 || options.quote_ < 0 || options.escape_ < 0
      || options.quote_ == '\n' || options.quote_ == '\r' || options.escape_ == '\n' || options.escape_ == '\r'
      || options.delimiter_ == options.quote_ || options.delimiter_ == '\n' || options.delimiter_ == '\r') {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(check_path(options.path_, secure_root, meta_allocator_, canonical_path_))) {
  } else if (OB_FAIL(ob_write_string(meta_allocator_, options.null_text_, options_.null_text_))) {
  } else {
    char root_buffer[PATH_MAX + 1];
    char root_path[PATH_MAX + 1];
    memcpy(root_buffer, secure_root.ptr(), secure_root.length());
    root_buffer[secure_root.length()] = '\0';
    int root_fd = ::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    int allowed_fd = -1;
    int input_fd = -1;
    if (::realpath(root_buffer, root_path) == nullptr || root_fd < 0) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else {
      allowed_fd = file_sql_open_beneath(root_fd, root_path + 1, true);
      int64_t root_length = strlen(root_path);
      const char *relative = canonical_path_.ptr() + root_length;
      if (*relative == '/') { ++relative; }
      if (allowed_fd < 0 || *relative == '\0') {
        ret = OB_ERR_NO_PRIVILEGE;
      } else {
        input_fd = file_sql_open_beneath(allowed_fd, relative, false);
        if (input_fd < 0 || ::fstat(input_fd, &snapshot_) != 0 || !S_ISREG(snapshot_.st_mode)) {
          ret = OB_ERR_NO_PRIVILEGE;
        } else if ((file_ = ::fdopen(input_fd, "rb")) == nullptr) {
          ret = OB_IO_ERROR;
        } else {
          input_fd = -1;
          unsigned char prefix[3];
          size_t length = ::fread(prefix, 1, 3, file_);
          data_offset_ = length == 3 && prefix[0] == 0xef && prefix[1] == 0xbb && prefix[2] == 0xbf ? 3 : 0;
          if (::fseeko(file_, data_offset_, SEEK_SET) != 0) { ret = OB_IO_ERROR; }
        }
      }
    }
    if (root_fd >= 0) { ::close(root_fd); }
    if (allowed_fd >= 0) { ::close(allowed_fd); }
    if (input_fd >= 0) { ::close(input_fd); }
  }
  if (OB_SUCC(ret) && options_.format_ == FileSqlFormat::PARQUET) {
    int fd = ::dup(::fileno(file_));
    if (fd < 0) { ret = OB_IO_ERROR; }
    else {
      auto reader = file_sql::open_parquet(fd);
      if (reader->status() != 0) {
        char message[384];
        snprintf(message, sizeof(message), "%.*s", static_cast<int>(reader->error().size()), reader->error().data());
        ret = error(message);
      }
      else { parquet_ = reader.into_raw(); }
    }
  }
  return ret;
}

int FileSqlReader::check_unchanged() const
{
  if (glob_ != nullptr) { return glob_->check_unchanged(); }
  int ret = OB_SUCCESS;
  struct stat current;
  struct stat named;
  if (file_ == nullptr || ::fstat(::fileno(file_), &current) != 0
      || ::stat(canonical_path_.ptr(), &named) != 0
      || !file_sql_same_file(snapshot_, current) || !file_sql_same_file(snapshot_, named)) {
    ret = error("input file changed during the query");
  }
  return ret;
}

void FileSqlReader::close()
{
  if (parquet_ != nullptr) {
    auto reader = rust::Box<file_sql::ParquetReader>::from_raw(static_cast<file_sql::ParquetReader *>(parquet_));
    parquet_ = nullptr;
  }
  if (glob_ != nullptr) { glob_->~FileSqlGlob(); glob_ = nullptr; }
  if (file_ != nullptr) { ::fclose(file_); file_ = nullptr; }
  cells_.reset();
  columns_.reset();
  row_allocator_.reset();
  meta_allocator_.reset();
  canonical_path_.reset();
  line_ = 0;
  record_line_ = 1;
  data_offset_ = 0;
  schema_ready_ = false;
}

bool FileSqlReader::valid_utf8(const ObString &value)
{
  bool valid = true;
  for (int64_t i = 0; valid && i < value.length();) {
    unsigned char first = value[i++];
    int count = 0;
    uint32_t code = 0;
    uint32_t minimum = 0;
    if (first < 0x80) { continue; }
    if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
    else { valid = false; }
    for (int j = 0; valid && j < count; ++j) {
      if (i >= value.length() || (static_cast<unsigned char>(value[i]) & 0xc0) != 0x80) { valid = false; }
      else { code = (code << 6) | (value[i++] & 0x3f); }
    }
    valid = valid && code >= minimum && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
  }
  return valid;
}

int FileSqlReader::read_csv()
{
  int ret = OB_SUCCESS;
  record_line_ = line_ + 1;
  cells_.reuse();
  row_allocator_.reuse();
  ObStringBuffer field(&row_allocator_);
  bool quoted = false;
  bool in_quotes = false;
  bool after_quote = false;
  bool any = false;
  int64_t bytes = 0;
  while (OB_SUCC(ret)) {
    int ch = ::fgetc(file_);
    if (++bytes > MAX_RECORD_BYTES || field.length() > MAX_CELL_BYTES) {
      ret = error("record or field exceeds the configured resource limit");
    } else if (ch == EOF && ::ferror(file_)) {
      ret = OB_IO_ERROR;
    } else if (ch == EOF && in_quotes) {
      ret = error("unterminated quoted CSV field");
    } else if (ch == EOF && !any) {
      ret = OB_ITER_END;
    } else if (in_quotes) {
      if (ch == '\n') { ++line_; }
      if (ch == options_.quote_) {
        if (options_.escape_ == options_.quote_) {
          int next = ::fgetc(file_);
          if (next == options_.quote_) { ret = field.append(reinterpret_cast<const char *>(&options_.quote_), 1); }
          else { if (next != EOF) { ::ungetc(next, file_); } in_quotes = false; after_quote = true; }
        } else { in_quotes = false; after_quote = true; }
      } else if (ch == options_.escape_ && options_.escape_ != options_.quote_) {
        int next = ::fgetc(file_);
        if (next == EOF) { ret = error("unterminated CSV escape"); }
        else { if (next == '\n') { ++line_; } char c = next; ret = field.append(&c, 1); }
      } else { char c = ch; ret = field.append(&c, 1); }
    } else if (ch == options_.delimiter_ || ch == '\n' || ch == '\r' || ch == EOF) {
      Cell cell;
      if (cells_.count() >= MAX_COLUMNS) { ret = error("too many columns"); }
      else if (OB_FAIL(field.get_result_string(cell.value_))) {
      } else if (!valid_utf8(cell.value_)) { ret = error("invalid UTF-8"); }
      else {
        cell.null_ = !quoted && cell.value_ == options_.null_text_;
        cell.type_ = cell.null_ ? FileSqlType::NIL : infer_type(cell.value_);
        ret = cells_.push_back(cell);
      }
      quoted = in_quotes = after_quote = false;
      if (ch != options_.delimiter_) {
        if (ch == '\r') { int next = ::fgetc(file_); if (next != '\n' && next != EOF) { ::ungetc(next, file_); } }
        ++line_;
        break;
      }
      any = true;
    } else if (after_quote) {
      ret = error("unexpected data after closing CSV quote");
    } else if (ch == options_.quote_) {
      if (field.length() != 0) { ret = error("quote inside an unquoted CSV field"); }
      else { quoted = in_quotes = true; any = true; }
    } else {
      char c = ch;
      ret = field.append(&c, 1);
      any = true;
    }
  }
  return ret;
}

bool FileSqlReader::parse_integer(const ObString &value, int64_t &number)
{
  int64_t i = 0;
  bool negative = !value.empty() && value[0] == '-';
  if (!value.empty() && (value[0] == '-' || value[0] == '+')) { ++i; }
  bool valid = i < value.length();
  uint64_t magnitude = 0;
  uint64_t limit = negative ? uint64_t(INT64_MAX) + 1 : INT64_MAX;
  for (; valid && i < value.length(); ++i) {
    uint64_t digit = static_cast<unsigned char>(value[i]) - '0';
    if (digit > 9 || magnitude > (limit - digit) / 10) { valid = false; }
    else { magnitude = magnitude * 10 + digit; }
  }
  if (valid) {
    number = negative ? (magnitude == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -static_cast<int64_t>(magnitude))
                      : static_cast<int64_t>(magnitude);
  }
  return valid;
}

int FileSqlReader::read_jsonl()
{
  int ret = OB_SUCCESS;
  bool found = false;
  while (OB_SUCC(ret) && !found) {
    record_line_ = line_ + 1;
    cells_.reuse();
    row_allocator_.reuse();
    ObStringBuffer buffer(&row_allocator_);
    int ch = 0;
    while (OB_SUCC(ret) && (ch = ::fgetc(file_)) != EOF && ch != '\n') {
      if (buffer.length() >= MAX_RECORD_BYTES) { ret = error("JSONL record is too large"); }
      else { char byte = ch; ret = buffer.append(&byte, 1); }
    }
    if (OB_FAIL(ret)) {
    } else if (ch == EOF && ::ferror(file_)) { ret = OB_IO_ERROR; }
    else if (ch == EOF && buffer.empty()) { ret = OB_ITER_END; }
    else {
      ++line_;
      bool blank = true;
      for (int64_t i = 0; blank && i < buffer.length(); ++i) {
        blank = buffer.ptr()[i] == ' ' || buffer.ptr()[i] == '\t' || buffer.ptr()[i] == '\r';
      }
      if (!blank) {
        ObIJsonBase *base = nullptr;
        bool in_string = false, escaped = false, root_seen = false;
        int64_t fields = 1, token_bytes = 0, string_bytes = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < buffer.length(); ++i) {
          char c = buffer.ptr()[i];
          if (in_string) {
            if (++string_bytes > MAX_CELL_BYTES) { ret = error("JSONL field exceeds the resource limit"); }
            else if (escaped) { escaped = false; }
            else if (c == '\\') { escaped = true; }
            else if (c == '"') { in_string = false; }
          } else if (c == '"') { in_string = true; string_bytes = 0; token_bytes = 0; }
          else if (c == '[' || (c == '{' && root_seen)) { ret = error("nested JSONL objects and arrays are not supported"); }
          else if (c == '{') { root_seen = true; token_bytes = 0; }
          else if (c == ',') { if (++fields > MAX_COLUMNS) { ret = error("too many JSONL fields"); } token_bytes = 0; }
          else if (c == ':' || c == '}' || c == ' ' || c == '\t' || c == '\r') { token_bytes = 0; }
          else if (++token_bytes > 256) { ret = error("JSONL scalar token exceeds the resource limit"); }
        }
        if (OB_FAIL(ret)) {
        } else if (!valid_utf8(buffer.string())) { ret = error("invalid UTF-8"); }
        else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&row_allocator_, buffer.string(),
                         ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
          ret = error("malformed JSONL object");
        } else if (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = error("JSONL records must be flat objects");
        } else {
          ObJsonObject *object = static_cast<ObJsonObject *>(base);
          if (object->element_count() > MAX_COLUMNS) { ret = error("too many JSONL fields"); }
          for (uint64_t i = 0; OB_SUCC(ret) && i < object->element_count(); ++i) {
            Cell cell;
            ObJsonNode *value = object->get_value(i);
            if (OB_FAIL(object->get_key(i, cell.name_))) {
            } else if (cell.name_.empty() || cell.name_.length() > 1024
                       || memchr(cell.name_.ptr(), '\0', cell.name_.length()) != nullptr) {
              ret = error("invalid JSONL field name");
            } else if (value == nullptr || value->json_type() == ObJsonNodeType::J_OBJECT
                       || value->json_type() == ObJsonNodeType::J_ARRAY) {
              ret = error("nested JSONL objects and arrays are not supported");
            } else if (value->json_type() == ObJsonNodeType::J_NULL) {
              cell.null_ = true;
            } else if (value->json_type() == ObJsonNodeType::J_STRING) {
              cell.value_.assign_ptr(value->get_data(), value->get_data_length());
              int64_t number = 0;
              cell.type_ = parse_datetime(cell.value_, true, number) ? FileSqlType::DATE
                         : parse_datetime(cell.value_, false, number) ? FileSqlType::DATETIME : FileSqlType::TEXT;
            } else {
              ObStringBuffer text(&row_allocator_);
              if (OB_FAIL(value->print(text, 0))) {
              } else if (OB_FAIL(text.get_result_string(cell.value_))) {
              } else {
                cell.type_ = value->json_type() == ObJsonNodeType::J_BOOLEAN ? FileSqlType::BOOL : infer_type(cell.value_);
              }
            }
            if (OB_SUCC(ret)) {
              if (cell.value_.length() > MAX_CELL_BYTES) { ret = error("JSONL field is too large"); }
              else if (OB_FAIL(cells_.push_back(cell))) {
              }
            }
          }
          found = OB_SUCC(ret);
        }
      }
    }
  }
  return ret;
}

int FileSqlReader::read_record()
{
  int ret = THIS_WORKER.is_timeout() ? OB_TIMEOUT : check_unchanged();
  if (OB_FAIL(ret)) {
  } else if (options_.format_ == FileSqlFormat::CSV) { ret = read_csv(); }
  else if (options_.format_ == FileSqlFormat::JSONL) { ret = read_jsonl(); }
  else { ret = OB_NOT_SUPPORTED; }
  if (ret == OB_SUCCESS || ret == OB_ITER_END) {
    int check = check_unchanged();
    if (check != OB_SUCCESS) { ret = check; }
  }
  return ret;
}

int FileSqlReader::add_column(const ObString &name, bool deduplicate, bool nullable)
{
  int ret = OB_SUCCESS;
  FileSqlColumn column;
  column.nullable_ = nullable;
  ObString candidate = name;
  char suffix[32];
  int64_t number = 1;
  bool unique = false;
  if (columns_.count() >= MAX_COLUMNS || name.length() > 1024) {
    ret = error("column name or schema exceeds the resource limit");
  } else if (!valid_utf8(name) || memchr(name.ptr(), '\0', name.length()) != nullptr) {
    ret = error("invalid column name");
  }
  while (OB_SUCC(ret) && !unique) {
    unique = true;
    for (int64_t i = 0; unique && i < columns_.count(); ++i) {
      unique = columns_.at(i).name_.case_compare(candidate) != 0;
    }
    if (!unique) {
      if (!deduplicate) { ret = error("JSONL field names collide under SQL identifier comparison"); }
      else {
        ObStringBuffer renamed(&meta_allocator_);
        snprintf(suffix, sizeof(suffix), "__%ld", ++number);
        if (OB_FAIL(renamed.append(name))) {
        } else if (OB_FAIL(renamed.append(suffix))) {
        } else if (OB_FAIL(renamed.get_result_string(candidate))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ob_write_string(meta_allocator_, candidate, column.name_))) {
    } else if (OB_FAIL(columns_.push_back(column))) {
    }
  }
  return ret;
}

int FileSqlReader::infer_schema()
{
  int ret = OB_SUCCESS;
  if (options_.format_ == FileSqlFormat::GLOB) {
    if (OB_FAIL(add_column(ObString("file"), false, false))) {
    } else { columns_.at(0).type_ = FileSqlType::TEXT; columns_.at(0).max_length_ = PATH_MAX; schema_ready_ = true; }
    return ret;
  } else if (options_.format_ == FileSqlFormat::PARQUET) {
    if (parquet_ == nullptr) { ret = OB_NOT_INIT; }
    else {
      auto &reader = *static_cast<file_sql::ParquetReader *>(parquet_);
      for (const auto &column : reader.columns()) {
        if (OB_SUCC(ret) && OB_FAIL(add_column(ObString(column.name.size(), column.name.data()), false, column.nullable))) {
        } else if (OB_SUCC(ret)) {
          columns_.at(columns_.count() - 1).type_ = static_cast<FileSqlType>(column.kind);
          columns_.at(columns_.count() - 1).max_length_ = column.kind == file_sql::Kind::Text ? MAX_CELL_BYTES : 0;
        }
      }
      if (OB_SUCC(ret)) { schema_ready_ = true; }
    }
    return ret;
  }
  int64_t rows = 0;
  bool first = true;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(read_record())) { break; }
    if (options_.format_ == FileSqlFormat::CSV) {
      if (first) {
        for (int64_t i = 0; OB_SUCC(ret) && i < cells_.count(); ++i) {
          char generated[32];
          snprintf(generated, sizeof(generated), "column%ld", i);
          ObString name = options_.header_ && !cells_.at(i).value_.empty()
                          ? cells_.at(i).value_ : ObString(generated);
          if (OB_FAIL(add_column(name, true, false))) {
          }
        }
      }
      if (OB_SUCC(ret) && !(first && options_.header_)) {
        if (cells_.count() != columns_.count()) { ret = error("CSV record has an inconsistent field count"); }
        for (int64_t i = 0; OB_SUCC(ret) && i < cells_.count(); ++i) {
          FileSqlColumn &column = columns_.at(i);
          column.type_ = widen(column.type_, cells_.at(i).type_);
          column.nullable_ |= cells_.at(i).null_;
          column.max_length_ = std::max<int64_t>(column.max_length_, cells_.at(i).value_.length());
        }
        ++rows;
      }
    } else {
      bool present[MAX_COLUMNS] = {};
      for (int64_t i = 0; OB_SUCC(ret) && i < cells_.count(); ++i) {
        Cell &cell = cells_.at(i);
        int64_t index = -1;
        for (int64_t j = 0; index < 0 && j < columns_.count(); ++j) {
          if (columns_.at(j).name_ == cell.name_) { index = j; }
        }
        if (index < 0) {
          if (OB_FAIL(add_column(cell.name_, false, rows > 0))) {
          } else { index = columns_.count() - 1; }
        }
        if (OB_SUCC(ret)) {
          present[index] = true;
          FileSqlColumn &column = columns_.at(index);
          column.type_ = widen(column.type_, cell.type_);
          column.nullable_ |= cell.null_;
          column.max_length_ = std::max<int64_t>(column.max_length_, cell.value_.length());
        }
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < columns_.count(); ++j) {
        columns_.at(j).nullable_ |= !present[j];
      }
      ++rows;
    }
    first = false;
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  if (OB_SUCC(ret)) {
    if (columns_.empty()) { ret = error("input has no columns to infer"); }
    else {
      schema_ready_ = true;
      ret = rescan();
    }
  }
  return ret;
}

int FileSqlReader::rescan()
{
  int ret = check_unchanged();
  if (OB_FAIL(ret)) {
  } else if (glob_ != nullptr) { ret = glob_->rescan(); }
  else if (parquet_ != nullptr) {
    auto &reader = *static_cast<file_sql::ParquetReader *>(parquet_);
    if (reader.rewind() != 0) { ret = error("could not rewind Parquet reader"); }
  } else if (::fseeko(file_, data_offset_, SEEK_SET) != 0) { ret = OB_IO_ERROR; }
  else {
    ::clearerr(file_);
    line_ = 0;
    if (options_.format_ == FileSqlFormat::CSV && options_.header_) {
      ret = read_record();
      if (OB_SUCC(ret) && cells_.count() != columns_.count()) { ret = error("CSV header changed"); }
    }
  }
  return ret;
}

int FileSqlReader::convert(const Cell &cell, const FileSqlColumn &column, ObObj &value)
{
  int ret = OB_SUCCESS;
  int64_t number = 0;
  double real = 0;
  if (cell.null_ || column.type_ == FileSqlType::NIL) { value.set_null(); }
  else if (column.type_ == FileSqlType::TEXT) {
    value.set_varchar(cell.value_);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else if (column.type_ == FileSqlType::BOOL) { value.set_tinyint(cell.value_.case_compare("true") == 0 ? 1 : 0); }
  else if (column.type_ == FileSqlType::I64) {
    if (!parse_integer(cell.value_, number)) { ret = error("invalid BIGINT value"); }
    else { value.set_int(number); }
  } else if (column.type_ == FileSqlType::F64) {
    if (!file_sql_parse_double(cell.value_, real)) { ret = error("invalid DOUBLE value"); }
    else { value.set_double(real); }
  } else if (column.type_ == FileSqlType::DATE) {
    if (!parse_datetime(cell.value_, true, number)) { ret = error("invalid DATE value"); }
    else { value.set_date(number); }
  } else if (column.type_ == FileSqlType::DATETIME) {
    if (!parse_datetime(cell.value_, false, number)) {
      if (!parse_datetime(cell.value_, true, number)) { ret = error("invalid DATETIME value"); }
      else { number *= 86400000000LL; }
    }
    if (OB_SUCC(ret)) { value.set_datetime(number); }
  }
  return ret;
}

int FileSqlReader::next_row(ObIArray<ObObj> &row)
{
  int ret = OB_SUCCESS;
  row.reuse();
  if (!schema_ready_) { ret = OB_NOT_INIT; FORWARD_USER_ERROR(ret, "File SQL row requested before schema binding"); }
  else if (glob_ != nullptr) {
    ObString path;
    ObObj value;
    if (OB_FAIL(glob_->next(path))) {
    } else {
      value.set_varchar(path); value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      ret = row.push_back(value);
    }
  } else if (parquet_ != nullptr) {
    auto &reader = *static_cast<file_sql::ParquetReader *>(parquet_);
    if (OB_FAIL(check_unchanged())) {
    } else {
      int status = reader.next();
      if (status == 1) { ret = OB_ITER_END; }
      else if (status != 0) {
        char message[384];
        snprintf(message, sizeof(message), "%.*s", static_cast<int>(reader.error().size()), reader.error().data());
        ret = error(message);
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) { ObObj value; value.set_null(); ret = row.push_back(value); }
        for (const auto &cell : reader.values()) {
          if (OB_SUCC(ret) && cell.column >= row.count()) { ret = OB_ERR_UNEXPECTED; }
          else if (OB_SUCC(ret)) {
            ObObj &value = row.at(cell.column);
            switch (cell.kind) {
              case file_sql::Kind::Null: value.set_null(); break;
              case file_sql::Kind::Integer: value.set_int(cell.integer); break;
              case file_sql::Kind::Double: value.set_double(cell.real); break;
              case file_sql::Kind::Boolean: value.set_tinyint(cell.integer); break;
              case file_sql::Kind::Date: value.set_date(cell.integer); break;
              case file_sql::Kind::DateTime: value.set_datetime(cell.integer); break;
              case file_sql::Kind::Text:
                value.set_varchar(ObString(cell.text.size(), cell.text.data())); value.set_collation_type(CS_TYPE_UTF8MB4_BIN); break;
              default: ret = OB_INVALID_DATA; break;
            }
          }
        }
      }
      int changed = check_unchanged();
      if (changed != OB_SUCCESS) { ret = changed; }
    }
  }
  else if (OB_FAIL(read_record())) {
  } else if (options_.format_ == FileSqlFormat::CSV && cells_.count() != columns_.count()) {
    ret = error("CSV record has an inconsistent field count");
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
      const Cell *cell = nullptr;
      Cell missing;
      missing.null_ = true;
      if (options_.format_ == FileSqlFormat::CSV) { cell = &cells_.at(i); }
      else {
        for (int64_t j = 0; cell == nullptr && j < cells_.count(); ++j) {
          if (cells_.at(j).name_ == columns_.at(i).name_) { cell = &cells_.at(j); }
        }
      }
      ObObj value;
      if (OB_FAIL(convert(cell == nullptr ? missing : *cell, columns_.at(i), value))) {
      } else if (OB_FAIL(row.push_back(value))) {
      }
    }
  }
  return ret;
}

int FileSqlReader::project(const ObIArray<int64_t> &indices)
{
  int ret = OB_SUCCESS;
  if (parquet_ != nullptr) {
    ObSEArray<uint32_t, 16> columns;
    for (int64_t i = 0; OB_SUCC(ret) && i < indices.count(); ++i) {
      if (indices.at(i) < 0 || indices.at(i) >= columns_.count()) { ret = OB_INVALID_ARGUMENT; }
      else { ret = columns.push_back(static_cast<uint32_t>(indices.at(i))); }
    }
    if (OB_SUCC(ret) && static_cast<file_sql::ParquetReader *>(parquet_)->project(
        rust::Slice<const uint32_t>(columns.get_data(), columns.count())) != 0) { ret = error("invalid Parquet projection"); }
  }
  return ret;
}

bool FileSqlReader::parse_datetime(const ObString &value, bool date_only, int64_t &number)
{
  bool valid = date_only ? value.length() == 10 : value.length() >= 19 && value.length() <= 26;
  int fields[6] = {};
  const int offsets[] = {0, 5, 8, 11, 14, 17};
  for (int i = 0; valid && i < (date_only ? 3 : 6); ++i) {
    int width = i == 0 ? 4 : 2;
    for (int j = 0; valid && j < width; ++j) {
      char c = value[offsets[i] + j];
      valid = c >= '0' && c <= '9';
      fields[i] = fields[i] * 10 + c - '0';
    }
  }
  if (valid) {
    int year = fields[0], month = fields[1], day = fields[2];
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    valid = value[4] == '-' && value[7] == '-' && year >= 1 && year <= 9999
            && month >= 1 && month <= 12 && day >= 1
            && day <= month_days[month - 1] + (month == 2 && leap ? 1 : 0);
    if (valid && !date_only) {
      valid = (value[10] == ' ' || value[10] == 'T') && value[13] == ':' && value[16] == ':'
              && fields[3] <= 23 && fields[4] <= 59 && fields[5] <= 59;
    }
    int64_t fraction = 0;
    if (valid && !date_only && value.length() > 19) {
      valid = value[19] == '.' && value.length() > 20;
      for (int64_t i = 20; valid && i < 26; ++i) {
        int digit = i < value.length() ? value[i] - '0' : 0;
        valid = digit >= 0 && digit <= 9;
        fraction = fraction * 10 + digit;
      }
    }
    if (valid) {
      int64_t y = year - 1;
      int64_t days = 365 * y + y / 4 - y / 100 + y / 400 - 719162 + day - 1;
      for (int i = 1; i < month; ++i) { days += month_days[i - 1] + (i == 2 && leap ? 1 : 0); }
      number = date_only ? days : ((days * 24 + fields[3]) * 3600 + fields[4] * 60 + fields[5]) * 1000000 + fraction;
    }
  }
  return valid;
}

FileSqlType FileSqlReader::infer_type(const ObString &value)
{
  int64_t integer = 0;
  double real = 0;
  bool integer_lexeme = !value.empty();
  int64_t start = !value.empty() && (value[0] == '-' || value[0] == '+') ? 1 : 0;
  integer_lexeme = start < value.length();
  for (int64_t i = start; integer_lexeme && i < value.length(); ++i) { integer_lexeme = value[i] >= '0' && value[i] <= '9'; }
  if (value.case_compare("true") == 0 || value.case_compare("false") == 0) { return FileSqlType::BOOL; }
  if (parse_integer(value, integer)) { return FileSqlType::I64; }
  if (integer_lexeme) { return FileSqlType::TEXT; }
  if (file_sql_parse_double(value, real)) { return FileSqlType::F64; }
  if (parse_datetime(value, true, integer)) { return FileSqlType::DATE; }
  if (parse_datetime(value, false, integer)) { return FileSqlType::DATETIME; }
  return FileSqlType::TEXT;
}

FileSqlType FileSqlReader::widen(FileSqlType left, FileSqlType right)
{
  if (left == FileSqlType::NIL) { return right; }
  if (right == FileSqlType::NIL || left == right) { return left; }
  if ((left == FileSqlType::I64 && right == FileSqlType::F64)
      || (right == FileSqlType::I64 && left == FileSqlType::F64)) { return FileSqlType::F64; }
  if ((left == FileSqlType::DATE && right == FileSqlType::DATETIME)
      || (right == FileSqlType::DATE && left == FileSqlType::DATETIME)) { return FileSqlType::DATETIME; }
  return FileSqlType::TEXT;
}

ObObjType FileSqlReader::object_type(FileSqlType type)
{
  const ObObjType types[] = {ObNullType, ObIntType, ObDoubleType, ObTinyIntType,
                            ObVarcharType, ObDateType, ObDateTimeType};
  return types[static_cast<int>(type)];
}
const char *FileSqlReader::type_name(FileSqlType type)
{
  const char *names[] = {"NULL", "BIGINT", "DOUBLE", "BOOLEAN", "VARCHAR", "DATE", "DATETIME"};
  return names[static_cast<int>(type)];
}

namespace {
int file_sql_json_integer(ObIAllocator &allocator, ObJsonObject &object, const char *key, int64_t value)
{
  ObJsonInt *node = OB_NEWx(ObJsonInt, &allocator, value);
  return node == nullptr ? OB_ALLOCATE_MEMORY_FAILED : object.add(ObString(key), node);
}
int file_sql_json_string(ObIAllocator &allocator, ObJsonObject &object, const char *key, const ObString &value)
{
  ObJsonString *node = OB_NEWx(ObJsonString, &allocator, value);
  return node == nullptr ? OB_ALLOCATE_MEMORY_FAILED : object.add(ObString(key), node);
}
int file_sql_json_get_integer(ObJsonObject &object, const char *key, int64_t &value)
{
  int ret = OB_SUCCESS;
  ObJsonNode *node = object.get_value(ObString(key));
  if (node == nullptr || (node->json_type() != ObJsonNodeType::J_INT
                         && node->json_type() != ObJsonNodeType::J_UINT)) { ret = OB_INVALID_DATA; }
  else if (node->json_type() == ObJsonNodeType::J_INT) { value = node->get_int(); }
  else if (node->get_uint() > INT64_MAX) { ret = OB_INVALID_DATA; }
  else { value = node->get_uint(); }
  return ret;
}
int file_sql_json_get_string(ObJsonObject &object, const char *key, ObString &value)
{
  int ret = OB_SUCCESS;
  ObJsonNode *node = object.get_value(ObString(key));
  if (node == nullptr || node->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_DATA; }
  else { value.assign_ptr(node->get_data(), node->get_data_length()); }
  return ret;
}
void file_sql_snapshot_fields(const struct stat &snapshot, int64_t (&fields)[7])
{
  fields[0] = snapshot.st_dev;
  fields[1] = snapshot.st_ino;
  fields[2] = snapshot.st_size;
#if defined(__APPLE__)
  fields[3] = snapshot.st_mtimespec.tv_sec;
  fields[4] = snapshot.st_mtimespec.tv_nsec;
  fields[5] = snapshot.st_ctimespec.tv_sec;
  fields[6] = snapshot.st_ctimespec.tv_nsec;
#else
  fields[3] = snapshot.st_mtim.tv_sec;
  fields[4] = snapshot.st_mtim.tv_nsec;
  fields[5] = snapshot.st_ctim.tv_sec;
  fields[6] = snapshot.st_ctim.tv_nsec;
#endif
}
} // namespace

int FileSqlReader::make_descriptor(ObIAllocator &allocator, ObString &descriptor) const
{
  int ret = OB_SUCCESS;
  ObJsonObject object(&allocator);
  ObJsonArray columns(&allocator);
  ObJsonArray fingerprint(&allocator);
  int64_t fields[7];
  file_sql_snapshot_fields(snapshot_, fields);
  if (!schema_ready_) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(file_sql_json_string(allocator, object, "path", canonical_path_))) {
  } else if (OB_FAIL(file_sql_json_string(allocator, object, "null", options_.null_text_))) {
  } else if (OB_FAIL(file_sql_json_integer(allocator, object, "format", static_cast<int>(options_.format_)))) {
  } else if (OB_FAIL(file_sql_json_integer(allocator, object, "header", options_.header_))) {
  } else if (OB_FAIL(file_sql_json_integer(allocator, object, "delimiter", options_.delimiter_))) {
  } else if (OB_FAIL(file_sql_json_integer(allocator, object, "quote", options_.quote_))) {
  } else if (OB_FAIL(file_sql_json_integer(allocator, object, "escape", options_.escape_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    const FileSqlColumn &column = columns_.at(i);
    ObJsonObject *entry = OB_NEWx(ObJsonObject, &allocator, &allocator);
    if (entry == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(file_sql_json_string(allocator, *entry, "name", column.name_))) {
    } else if (OB_FAIL(file_sql_json_integer(allocator, *entry, "type", static_cast<int>(column.type_)))) {
    } else if (OB_FAIL(file_sql_json_integer(allocator, *entry, "nullable", column.nullable_))) {
    } else if (OB_FAIL(file_sql_json_integer(allocator, *entry, "length", column.max_length_))) {
    } else if (OB_FAIL(columns.append(entry))) {
    }
  }
  for (int i = 0; OB_SUCC(ret) && i < 7; ++i) {
    ObJsonInt *value = OB_NEWx(ObJsonInt, &allocator, fields[i]);
    if (value == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(fingerprint.append(value))) {
    }
  }
  ObStringBuffer buffer(&allocator);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(object.add("columns", &columns))) {
  } else if (OB_FAIL(object.add("fingerprint", &fingerprint))) {
  } else if (OB_FAIL(object.print(buffer, 0))) {
  } else if (OB_FAIL(buffer.get_result_string(descriptor))) {
  }
  return ret;
}

int FileSqlReader::descriptor_column_names(const ObString &descriptor, ObIAllocator &allocator,
                                          ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator temporary("FileSqlNames");
  ObIJsonBase *base = nullptr;
  if (descriptor.length() > 2 * 1024 * 1024) { ret = OB_INVALID_DATA; }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&temporary, descriptor, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
  } else if (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
  else {
    ObJsonNode *columns = static_cast<ObJsonObject *>(base)->get_value("columns");
    if (columns == nullptr || columns->json_type() != ObJsonNodeType::J_ARRAY || columns->element_count() > MAX_COLUMNS) { ret = OB_INVALID_DATA; }
    else {
      for (uint64_t i = 0; OB_SUCC(ret) && i < columns->element_count(); ++i) {
        ObJsonNode *column = static_cast<ObJsonArray *>(columns)->get_value(i);
        ObString name, copy;
        if (column == nullptr || column->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
        else if (OB_FAIL(file_sql_json_get_string(*static_cast<ObJsonObject *>(column), "name", name))) {
        } else if (name.empty() || name.length() > 1024) { ret = OB_INVALID_DATA; }
        else if (OB_FAIL(ob_write_string(allocator, name, copy))) {
        } else { ret = names.push_back(copy); }
      }
    }
  }
  return ret;
}

int FileSqlReader::open_descriptor(const ObString &descriptor, const ObString &secure_root)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator temporary("FileSqlBind");
  ObIJsonBase *base = nullptr;
  FileSqlOptions options;
  int64_t format = 0, header = 0, delimiter = 0, quote = 0, escape = 0;
  if (descriptor.length() > 2 * 1024 * 1024) { ret = OB_INVALID_DATA; }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&temporary, descriptor,
          ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
  } else if (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
  else {
    ObJsonObject &object = *static_cast<ObJsonObject *>(base);
    if (OB_FAIL(file_sql_json_get_string(object, "path", options.path_))) {
    } else if (OB_FAIL(file_sql_json_get_string(object, "null", options.null_text_))) {
    } else if (OB_FAIL(file_sql_json_get_integer(object, "format", format))) {
    } else if (OB_FAIL(file_sql_json_get_integer(object, "header", header))) {
    } else if (OB_FAIL(file_sql_json_get_integer(object, "delimiter", delimiter))) {
    } else if (OB_FAIL(file_sql_json_get_integer(object, "quote", quote))) {
    } else if (OB_FAIL(file_sql_json_get_integer(object, "escape", escape))) {
    } else if (format < 0 || format > static_cast<int>(FileSqlFormat::GLOB)
               || header < 0 || header > 1 || delimiter <= 0 || delimiter > 127
               || quote <= 0 || quote > 127 || escape <= 0 || escape > 127) { ret = OB_INVALID_DATA; }
    else {
      options.format_ = static_cast<FileSqlFormat>(format);
      options.header_ = header;
      options.delimiter_ = delimiter;
      options.quote_ = quote;
      options.escape_ = escape;
      ret = open(options, secure_root);
    }
    ObJsonNode *columns = object.get_value("columns");
    ObJsonNode *fingerprint = object.get_value("fingerprint");
    if (OB_FAIL(ret)) {
    } else if (columns == nullptr || columns->json_type() != ObJsonNodeType::J_ARRAY
               || columns->element_count() == 0 || columns->element_count() > MAX_COLUMNS
               || fingerprint == nullptr || fingerprint->json_type() != ObJsonNodeType::J_ARRAY
               || fingerprint->element_count() != 7) { ret = OB_INVALID_DATA; }
    else {
      int64_t fields[7];
      file_sql_snapshot_fields(snapshot_, fields);
      for (int i = 0; OB_SUCC(ret) && i < 7; ++i) {
        ObJsonNode *field = static_cast<ObJsonArray *>(fingerprint)->get_value(i);
        if (field == nullptr || field->json_type() != ObJsonNodeType::J_INT || field->get_int() != fields[i]) {
          ret = error("input file changed after schema binding");
        }
      }
      for (uint64_t i = 0; OB_SUCC(ret) && i < columns->element_count(); ++i) {
        ObJsonNode *entry = static_cast<ObJsonArray *>(columns)->get_value(i);
        ObString name;
        int64_t type = 0, nullable = 0, length = 0;
        if (entry == nullptr || entry->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
        else {
          ObJsonObject &column = *static_cast<ObJsonObject *>(entry);
          if (OB_FAIL(file_sql_json_get_string(column, "name", name))) {
          } else if (OB_FAIL(file_sql_json_get_integer(column, "type", type))) {
          } else if (OB_FAIL(file_sql_json_get_integer(column, "nullable", nullable))) {
          } else if (OB_FAIL(file_sql_json_get_integer(column, "length", length))) {
          } else if (type < 0 || type > static_cast<int>(FileSqlType::DATETIME)
                     || nullable < 0 || nullable > 1 || length < 0 || length > MAX_CELL_BYTES) { ret = OB_INVALID_DATA; }
          else if (OB_FAIL(add_column(name, false, nullable))) {
          } else {
            columns_.at(columns_.count() - 1).type_ = static_cast<FileSqlType>(type);
            columns_.at(columns_.count() - 1).max_length_ = length;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) { schema_ready_ = true; ret = rescan(); }
  return ret;
}

} } // namespace oceanbase::sql

#else
namespace oceanbase { namespace sql {
using namespace common;
FileSqlReader::FileSqlReader()
  : meta_allocator_("FileSqlMeta"), row_allocator_("FileSqlRow"), file_(nullptr),
    line_(0), record_line_(1), data_offset_(0), schema_ready_(false), parquet_(nullptr), glob_(nullptr) {}
FileSqlReader::~FileSqlReader() { close(); }
void FileSqlReader::close() { cells_.reset(); columns_.reset(); row_allocator_.reset(); meta_allocator_.reset(); }
int FileSqlReader::open(const FileSqlOptions &, const ObString &) { return OB_NOT_SUPPORTED; }
int FileSqlReader::infer_schema() { return OB_NOT_SUPPORTED; }
int FileSqlReader::make_descriptor(ObIAllocator &, ObString &) const { return OB_NOT_SUPPORTED; }
int FileSqlReader::open_descriptor(const ObString &, const ObString &) { return OB_NOT_SUPPORTED; }
int FileSqlReader::next_row(ObIArray<ObObj> &) { return OB_NOT_SUPPORTED; }
int FileSqlReader::project(const ObIArray<int64_t> &) { return OB_NOT_SUPPORTED; }
int FileSqlReader::rescan() { return OB_NOT_SUPPORTED; }
bool FileSqlReader::valid_utf8(const ObString &) { return false; }
int FileSqlReader::descriptor_column_names(const ObString &, ObIAllocator &, ObIArray<ObString> &) { return OB_NOT_SUPPORTED; }
int FileSqlReader::check_path(const ObString &, const ObString &, ObIAllocator &, ObString &) { return OB_NOT_SUPPORTED; }
ObObjType FileSqlReader::object_type(FileSqlType type) {
  const ObObjType types[] = {ObNullType, ObIntType, ObDoubleType, ObTinyIntType, ObVarcharType, ObDateType, ObDateTimeType};
  return types[static_cast<int>(type)];
}
const char *FileSqlReader::type_name(FileSqlType type) {
  const char *names[] = {"NULL", "BIGINT", "DOUBLE", "BOOLEAN", "VARCHAR", "DATE", "DATETIME"};
  return names[static_cast<int>(type)];
}
} }
#endif
