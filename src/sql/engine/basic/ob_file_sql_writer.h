/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase { namespace sql {

// A COPY destination is published only after all input rows succeed. Destruction
// removes any unpublished temporary file through its pinned parent directory.
class FileSqlWriter {
public:
  FileSqlWriter();
  ~FileSqlWriter();
  static bool is_copy(const common::ObString &descriptor);
  static int encode_options(common::ObIAllocator &allocator, const FileSqlOptions &options,
                            common::ObString &descriptor);
  static int decode_options(const common::ObString &descriptor, common::ObIAllocator &allocator,
                            FileSqlOptions &options);
  int open(const common::ObString &path, const common::ObString &secure_root,
           const FileSqlOptions &options, const common::ObIArray<common::ObString> &names);
  int write_row(const common::ObIArray<common::ObObj> &row);
  int commit();
  void abort();
private:
  int append(const char *data, int64_t length);
  int write_field(const common::ObString &value, bool is_null);
  int check_parent() const;
  common::ObArenaAllocator allocator_;
  FileSqlOptions options_;
  FILE *file_;
  int directory_;
  common::ObString parent_;
  struct stat parent_snapshot_;
  char target_[256];
  char temporary_[96];
  int64_t row_bytes_;
  bool committed_;
  DISALLOW_COPY_AND_ASSIGN(FileSqlWriter);
};
} }
