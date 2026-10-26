/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_sql_writer.h"
#if !defined(_WIN32)
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include "lib/string/ob_string_buffer.h"
#include "common/json_type/ob_json_tree.h"
#include "share/ob_errno.h"

namespace oceanbase { namespace sql {
using namespace common;
namespace {
const char FILE_COPY_PREFIX[] = "native_csv_copy:";
// Every opened component is a directory, with symlink traversal disabled.
int file_copy_open_parent(const char *path)
{
  int fd = ::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  const char *part = path + 1;
  while (fd >= 0 && *part != '\0') {
    const char *end = strchr(part, '/');
    int64_t length = end == nullptr ? strlen(part) : end - part;
    char name[NAME_MAX + 1];
    if (length <= 0 || length > NAME_MAX) { ::close(fd); fd = -1; break; }
    memcpy(name, part, length); name[length] = '\0';
    int next = ::openat(fd, name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    ::close(fd); fd = next;
    if (end == nullptr) { break; }
    part = end + 1;
  }
  return fd;
}
}

FileSqlWriter::FileSqlWriter() : allocator_("FileSqlCopy"), file_(nullptr), directory_(-1),
                                row_bytes_(0), committed_(false)
{ target_[0] = '\0'; temporary_[0] = '\0'; memset(&parent_snapshot_, 0, sizeof(parent_snapshot_)); }
FileSqlWriter::~FileSqlWriter() { abort(); }

bool FileSqlWriter::is_copy(const ObString &descriptor)
{
  return descriptor.length() >= static_cast<int64_t>(sizeof(FILE_COPY_PREFIX) - 1)
      && memcmp(descriptor.ptr(), FILE_COPY_PREFIX, sizeof(FILE_COPY_PREFIX) - 1) == 0;
}
int FileSqlWriter::encode_options(ObIAllocator &allocator, const FileSqlOptions &options, ObString &descriptor)
{
  int ret = OB_SUCCESS;
  ObJsonObject object(&allocator);
  ObJsonInt header(options.header_), delimiter(options.delimiter_), quote(options.quote_), escape(options.escape_);
  ObJsonString null_text(options.null_text_);
  ObStringBuffer buffer(&allocator);
  if (OB_FAIL(object.add("header", &header))) {
  } else if (OB_FAIL(object.add("delimiter", &delimiter))) {
  } else if (OB_FAIL(object.add("quote", &quote))) {
  } else if (OB_FAIL(object.add("escape", &escape))) {
  } else if (OB_FAIL(object.add("null", &null_text))) {
  } else if (OB_FAIL(buffer.append(FILE_COPY_PREFIX))) {
  } else if (OB_FAIL(object.print(buffer, 0))) {
  } else if (OB_FAIL(buffer.get_result_string(descriptor))) {
  }
  return ret;
}
int FileSqlWriter::decode_options(const ObString &descriptor, ObIAllocator &allocator, FileSqlOptions &options)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;
  ObString json;
  if (!is_copy(descriptor)) { ret = OB_INVALID_ARGUMENT; }
  else { json.assign_ptr(descriptor.ptr() + sizeof(FILE_COPY_PREFIX) - 1,
                        static_cast<int64_t>(descriptor.length() - sizeof(FILE_COPY_PREFIX) + 1)); }
  if (OB_FAIL(ret)) {
  }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, json, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
  } else if (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
  else {
    ObJsonObject &object = *static_cast<ObJsonObject *>(base);
    const char *names[] = {"header", "delimiter", "quote", "escape"};
    int64_t values[4] = {};
    for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
      ObJsonNode *value = object.get_value(names[i]);
      if (value == nullptr || value->json_type() != ObJsonNodeType::J_INT) { ret = OB_INVALID_DATA; }
      else { values[i] = value->get_int(); }
    }
    ObJsonNode *null_text = object.get_value("null");
    if (OB_FAIL(ret)) {
    } else if (values[0] < 0 || values[0] > 1 || values[1] <= 0 || values[1] > 127
               || values[2] <= 0 || values[2] > 127 || values[3] <= 0 || values[3] > 127
               || null_text == nullptr || null_text->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_DATA; }
    else {
      options.header_ = values[0]; options.delimiter_ = values[1]; options.quote_ = values[2]; options.escape_ = values[3];
      options.null_text_ = ObString(null_text->get_data_length(), null_text->get_data());
    }
  }
  return ret;
}

int FileSqlWriter::open(const ObString &path, const ObString &secure_root,
                        const FileSqlOptions &options, const ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  abort(); committed_ = false; options_ = options;
  const char *separator = path.reverse_find('/');
  int64_t slash = separator == nullptr ? -1 : separator - path.ptr();
  ObString parent = slash < 0 ? ObString(".") : ObString(slash == 0 ? 1 : slash, path.ptr());
  ObString target(path.length() - slash - 1, path.ptr() + slash + 1);
  if (path.empty() || memchr(path.ptr(), '\0', path.length()) != nullptr || target.empty()
      || target.length() > NAME_MAX || target == "." || target == ".."
      || options.delimiter_ <= 0 || options.quote_ <= 0 || options.escape_ <= 0
      || options.delimiter_ == options.quote_ || options.delimiter_ == '\n' || options.delimiter_ == '\r'
      || options.quote_ == '\n' || options.quote_ == '\r' || options.escape_ == '\n' || options.escape_ == '\r'
      || options.null_text_.length() > 4 * 1024 * 1024
      || !FileSqlReader::valid_utf8(options.null_text_)) { ret = OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; OB_SUCC(ret) && i < options.null_text_.length(); ++i) {
    char c = options.null_text_[i];
    if (c == options.delimiter_ || c == options.quote_ || c == '\n' || c == '\r' || c == '\0') { ret = OB_INVALID_ARGUMENT; }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(FileSqlReader::check_path(parent, secure_root, allocator_, parent_))) {
  } else if ((directory_ = file_copy_open_parent(parent_.ptr())) < 0 || ::fstat(directory_, &parent_snapshot_) != 0) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else {
    memcpy(target_, target.ptr(), target.length()); target_[target.length()] = '\0';
    struct stat existing;
    if (::fstatat(directory_, target_, &existing, AT_SYMLINK_NOFOLLOW) == 0) { ret = OB_FILE_ALREADY_EXIST; }
    else if (errno != ENOENT) { ret = OB_IO_ERROR; }
    else {
      static std::atomic<uint64_t> serial{0};
      int fd = -1;
      for (int attempt = 0; fd < 0 && OB_SUCC(ret) && attempt < 32; ++attempt) {
        snprintf(temporary_, sizeof(temporary_), ".seekdb-copy-%ld-%lu.tmp", static_cast<long>(getpid()), ++serial);
        fd = ::openat(directory_, temporary_, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) { ret = OB_IO_ERROR; }
      }
      if (fd < 0) { temporary_[0] = '\0'; ret = OB_IO_ERROR; }
      else if ((file_ = ::fdopen(fd, "wb")) == nullptr) { ::close(fd); ret = OB_IO_ERROR; }
      if (OB_SUCC(ret) && options.header_) {
        row_bytes_ = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
          if (i > 0) { ret = append(&options.delimiter_, 1); }
          if (OB_SUCC(ret)) { ret = write_field(names.at(i), false); }
        }
        if (OB_SUCC(ret)) { ret = append("\n", 1); }
      }
    }
  }
  if (OB_FAIL(ret)) {
    abort();
    if (ret == OB_INVALID_ARGUMENT) { FORWARD_USER_ERROR(ret, "COPY output path or CSV options are invalid"); }
  }
  return ret;
}
int FileSqlWriter::append(const char *data, int64_t length)
{
  int ret = OB_SUCCESS;
  if (length < 0 || row_bytes_ > 16 * 1024 * 1024 - length) { ret = OB_SIZE_OVERFLOW; }
  else if (file_ == nullptr) { ret = OB_NOT_INIT; }
  else if (length > 0 && ::fwrite(data, 1, length, file_) != static_cast<size_t>(length)) { ret = OB_IO_ERROR; }
  else { row_bytes_ += length; }
  return ret;
}
int FileSqlWriter::write_field(const ObString &value, bool is_null)
{
  int ret = OB_SUCCESS;
  if (is_null) { ret = append(options_.null_text_.ptr(), options_.null_text_.length()); }
  else if (value.length() > 4 * 1024 * 1024) { ret = OB_SIZE_OVERFLOW; }
  else if (!FileSqlReader::valid_utf8(value)) { ret = OB_INVALID_DATA; }
  else {
    bool quote = value == options_.null_text_;
    for (int64_t i = 0; !quote && i < value.length(); ++i) {
      char c = value[i];
      quote = c == options_.delimiter_ || c == options_.quote_ || c == options_.escape_ || c == '\n' || c == '\r';
    }
    if (quote) { ret = append(&options_.quote_, 1); }
    int64_t start = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      if (quote && (value[i] == options_.quote_ || (options_.escape_ != options_.quote_ && value[i] == options_.escape_))) {
        if (OB_FAIL(append(value.ptr() + start, i - start))) {
        } else if (OB_FAIL(append(&options_.escape_, 1))) {
        }
        start = i;
      }
    }
    if (OB_SUCC(ret)) { ret = append(value.ptr() + start, value.length() - start); }
    if (OB_SUCC(ret) && quote) { ret = append(&options_.quote_, 1); }
  }
  return ret;
}
int FileSqlWriter::write_row(const ObIArray<ObObj> &row)
{
  int ret = OB_SUCCESS;
  row_bytes_ = 0;
  if (OB_FAIL(check_parent())) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < row.count(); ++i) {
    const ObObj &value = row.at(i);
    ObString text;
    char buffer[4096];
    int64_t pos = 0;
    if (i > 0) { ret = append(&options_.delimiter_, 1); }
    if (OB_FAIL(ret)) {
    } else if (value.is_null()) { ret = write_field(text, true); }
    else {
      if (ob_is_string_type(value.get_type()) && !is_lob_storage(value.get_type())) { text = value.get_string(); }
      else if (value.is_tinyint() && (value.get_tinyint() == 0 || value.get_tinyint() == 1)) { text = value.get_tinyint() ? ObString("true") : ObString("false"); }
      else if (value.is_ext() || value.is_json() || is_lob_storage(value.get_type()) || value.is_geometry()) { ret = OB_NOT_SUPPORTED; }
      else if (OB_FAIL(value.print_plain_str_literal(buffer, sizeof(buffer), pos))) {
      } else { text.assign_ptr(buffer, pos); }
      if (OB_SUCC(ret)) { ret = write_field(text, false); }
    }
  }
  if (OB_SUCC(ret)) { ret = append("\n", 1); }
  return ret;
}
int FileSqlWriter::check_parent() const
{
  struct stat current;
  struct stat named;
  return directory_ < 0 || ::fstat(directory_, &current) != 0 || ::stat(parent_.ptr(), &named) != 0
      || current.st_dev != parent_snapshot_.st_dev || current.st_ino != parent_snapshot_.st_ino
      || named.st_dev != current.st_dev || named.st_ino != current.st_ino ? OB_IO_ERROR : OB_SUCCESS;
}
int FileSqlWriter::commit()
{
  int ret = OB_SUCCESS;
  if (file_ == nullptr || committed_) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(check_parent())) {
  } else {
    if (::fflush(file_) != 0 || ::fsync(::fileno(file_)) != 0) { ret = OB_IO_ERROR; }
    if (::fclose(file_) != 0 && OB_SUCC(ret)) { ret = OB_IO_ERROR; }
    file_ = nullptr;
    if (OB_SUCC(ret)) {
      // linkat is an atomic publish that never replaces an existing target.
      if (::linkat(directory_, temporary_, directory_, target_, 0) != 0) { ret = errno == EEXIST ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR; }
      else { committed_ = true; }
    }
  }
  abort();
  return ret;
}
void FileSqlWriter::abort()
{
  if (file_ != nullptr) { ::fclose(file_); file_ = nullptr; }
  if (directory_ >= 0) {
    if (temporary_[0] != '\0') { ::unlinkat(directory_, temporary_, 0); }
    ::close(directory_); directory_ = -1;
  }
  temporary_[0] = '\0';
}
} }

#else
namespace oceanbase { namespace sql {
using namespace common;
FileSqlWriter::FileSqlWriter() : allocator_("FileSqlCopy"), file_(nullptr), directory_(-1), row_bytes_(0), committed_(false) {}
FileSqlWriter::~FileSqlWriter() {}
bool FileSqlWriter::is_copy(const ObString &value) { return value.prefix_match("native_csv_copy:"); }
int FileSqlWriter::encode_options(ObIAllocator &, const FileSqlOptions &, ObString &) { return OB_NOT_SUPPORTED; }
int FileSqlWriter::decode_options(const ObString &, ObIAllocator &, FileSqlOptions &) { return OB_NOT_SUPPORTED; }
int FileSqlWriter::open(const ObString &, const ObString &, const FileSqlOptions &, const ObIArray<ObString> &) { return OB_NOT_SUPPORTED; }
int FileSqlWriter::write_row(const ObIArray<ObObj> &) { return OB_NOT_SUPPORTED; }
int FileSqlWriter::commit() { return OB_NOT_SUPPORTED; }
void FileSqlWriter::abort() {}
} }
#endif
