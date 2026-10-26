/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#if !defined(_WIN32)
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_sql_glob.h"
#include <cerrno>
#include <fcntl.h>
#include <fnmatch.h>
#include <unistd.h>
#include "lib/worker.h"

namespace oceanbase { namespace sql {
using namespace common;
FileSqlGlob::FileSqlGlob() : allocator_("FileSqlGlob"), depth_(-1), matches_(0)
{ memset(&snapshot_, 0, sizeof(snapshot_)); }
FileSqlGlob::~FileSqlGlob() { close(); }
void FileSqlGlob::close()
{
  for (auto &frame : frames_) {
    if (frame.directory_ != nullptr) { ::closedir(frame.directory_); frame.directory_ = nullptr; }
  }
  depth_ = -1; matches_ = 0;
}
int FileSqlGlob::open(const ObString &pattern, const ObString &secure_root)
{
  int ret = OB_SUCCESS;
  close(); components_.reset(); allocator_.reset();
  int64_t split = -1;
  bool wildcard = false;
  for (int64_t i = 0; i < pattern.length() && !wildcard; ++i) {
    if (pattern[i] == '/') { split = i; }
    if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[') { wildcard = true; }
  }
  ObString parent = split < 0 ? ObString(".") : ObString(split == 0 ? 1 : split, pattern.ptr());
  if (pattern.empty() || pattern.length() > PATH_MAX || memchr(pattern.ptr(), '\0', pattern.length()) != nullptr) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(FileSqlReader::check_path(parent, secure_root, allocator_, base_))) {
  } else {
    char canonical[PATH_MAX + 1];
    int written = snprintf(canonical, sizeof(canonical), "%.*s%s%.*s", base_.length(), base_.ptr(),
        base_ == "/" ? "" : "/", static_cast<int>(pattern.length() - split - 1), pattern.ptr() + split + 1);
    if (written < 0 || written > PATH_MAX) { ret = OB_SIZE_OVERFLOW; }
    else if (OB_FAIL(ob_write_string(allocator_, ObString(written, canonical), pattern_, true))) {
    } else {
      const char *part = pattern_.ptr() + base_.length();
      if (*part == '/') { ++part; }
      while (OB_SUCC(ret) && *part != '\0') {
        const char *end = strchr(part, '/');
        int64_t length = end == nullptr ? strlen(part) : end - part;
        ObString component(length, part);
        ObString stored;
        if (length <= 0 || length > NAME_MAX || component == "." || component == ".." || components_.count() >= MAX_DEPTH) { ret = OB_INVALID_ARGUMENT; }
        else if (OB_FAIL(ob_write_string(allocator_, component, stored, true))) {
        } else if (OB_FAIL(components_.push_back(stored))) {
        }
        if (end == nullptr) { break; }
        part = end + 1;
      }
      if (OB_SUCC(ret) && (::stat(base_.ptr(), &snapshot_) != 0 || !S_ISDIR(snapshot_.st_mode))) { ret = OB_INVALID_ARGUMENT; }
      if (OB_SUCC(ret)) { ret = rescan(); }
    }
  }
  return ret;
}
int FileSqlGlob::check_unchanged() const
{
  struct stat current;
  return ::stat(base_.ptr(), &current) != 0 || current.st_dev != snapshot_.st_dev
      || current.st_ino != snapshot_.st_ino ? OB_IO_ERROR : OB_SUCCESS;
}
int FileSqlGlob::rescan()
{
  int ret = OB_SUCCESS;
  close();
  if (components_.empty()) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(check_unchanged())) {
  } else {
    // Open the absolute canonical directory without following replacement links.
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const char *part = base_.ptr() + 1;
    while (fd >= 0 && *part != '\0') {
      const char *end = strchr(part, '/');
      int64_t length = end == nullptr ? strlen(part) : end - part;
      char name[NAME_MAX + 1];
      if (length <= 0 || length > NAME_MAX) { ::close(fd); fd = -1; break; }
      memcpy(name, part, length); name[length] = '\0';
      int next = ::openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      ::close(fd); fd = next;
      if (end == nullptr) { break; }
      part = end + 1;
    }
    struct stat opened;
    if (fd < 0 || ::fstat(fd, &opened) != 0 || opened.st_dev != snapshot_.st_dev || opened.st_ino != snapshot_.st_ino) {
      if (fd >= 0) { ::close(fd); } ret = OB_IO_ERROR;
    } else if ((frames_[0].directory_ = ::fdopendir(fd)) == nullptr) { ::close(fd); ret = OB_IO_ERROR; }
    else {
      depth_ = 0; frames_[0].component_ = 0;
      snprintf(frames_[0].path_, sizeof(frames_[0].path_), "%.*s", base_.length(), base_.ptr());
    }
  }
  return ret;
}
int FileSqlGlob::next(ObString &path)
{
  int ret = OB_SUCCESS;
  bool found = false;
  if (OB_FAIL(check_unchanged())) {
  }
  while (OB_SUCC(ret) && !found && depth_ >= 0) {
    Frame &frame = frames_[depth_];
    if (THIS_WORKER.is_timeout()) { ret = OB_TIMEOUT; break; }
    struct stat current, named;
    if (::fstat(::dirfd(frame.directory_), &current) != 0 || ::stat(frame.path_, &named) != 0
        || current.st_dev != named.st_dev || current.st_ino != named.st_ino) { ret = OB_IO_ERROR; break; }
    errno = 0;
    struct dirent *entry = ::readdir(frame.directory_);
    if (entry == nullptr) {
      if (errno != 0) { ret = OB_IO_ERROR; }
      ::closedir(frame.directory_); frame.directory_ = nullptr; --depth_;
    } else if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0
               && ::fnmatch(components_.at(frame.component_).ptr(), entry->d_name, FNM_PERIOD) == 0) {
      bool final = frame.component_ == components_.count() - 1;
      int fd = ::openat(::dirfd(frame.directory_), entry->d_name,
                       O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));
      struct stat child;
      if (fd < 0) { continue; } // links, disappearing entries and non-directories
      if (::fstat(fd, &child) != 0) { ::close(fd); ret = OB_IO_ERROR; }
      else if (final) {
        ::close(fd);
        if (S_ISREG(child.st_mode)) {
          int written = snprintf(result_, sizeof(result_), "%s%s%s", frame.path_, strcmp(frame.path_, "/") == 0 ? "" : "/", entry->d_name);
          if (written < 0 || written > PATH_MAX || ++matches_ > 1000000) { ret = OB_SIZE_OVERFLOW; }
          else { path.assign_ptr(result_, written); found = true; }
        }
      } else if (depth_ + 1 >= MAX_DEPTH) { ::close(fd); ret = OB_SIZE_OVERFLOW; }
      else {
        Frame &next = frames_[depth_ + 1];
        int written = snprintf(next.path_, sizeof(next.path_), "%s%s%s", frame.path_, strcmp(frame.path_, "/") == 0 ? "" : "/", entry->d_name);
        if (written < 0 || written > PATH_MAX) { ::close(fd); ret = OB_SIZE_OVERFLOW; }
        else if ((next.directory_ = ::fdopendir(fd)) == nullptr) { ::close(fd); ret = OB_IO_ERROR; }
        else { next.component_ = frame.component_ + 1; ++depth_; }
      }
    }
  }
  if (OB_SUCC(ret) && !found) { ret = OB_ITER_END; }
  return ret;
}
} }

#endif
