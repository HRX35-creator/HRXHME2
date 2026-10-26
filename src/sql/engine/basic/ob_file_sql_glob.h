/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <dirent.h>
#include <limits.h>
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase { namespace sql {
class FileSqlGlob {
public:
  FileSqlGlob();
  ~FileSqlGlob();
  int open(const common::ObString &pattern, const common::ObString &secure_root);
  int next(common::ObString &path);
  int rescan();
  void close();
  const struct stat &snapshot() const { return snapshot_; }
  const common::ObString &pattern() const { return pattern_; }
  int check_unchanged() const;
private:
  static constexpr int MAX_DEPTH = 32;
  struct Frame { DIR *directory_ = nullptr; int component_ = 0; char path_[PATH_MAX + 1]; };
  Frame frames_[MAX_DEPTH];
  common::ObArenaAllocator allocator_;
  common::ObSEArray<common::ObString, 8> components_;
  common::ObString base_;
  common::ObString pattern_;
  struct stat snapshot_;
  int depth_;
  int64_t matches_;
  char result_[PATH_MAX + 1];
  DISALLOW_COPY_AND_ASSIGN(FileSqlGlob);
};
} }
