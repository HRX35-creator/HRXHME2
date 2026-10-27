/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <atomic>
#include "share/ob_thread_pool.h"
#include "lib/string/ob_string.h"

namespace oceanbase {
namespace common { class ObMySQLProxy; class ObIAllocator; }
namespace omt {
class AIColumnWorker final : public share::ObThreadPool
{
public:
  AIColumnWorker();
  ~AIColumnWorker() override;
  int start();
  void stop() override;
  void wait() override;
  void run1() override;
private:
  struct Group;
  struct Task;
  int reclaim_previous_boot();
  int process_one();
  int process_group(const Group &group);
  int execute_task(const Group &group, const Task &task);
  int finish_group(const Group &group);
  int release_group(const Group &group);
  int save_outcome(const Group &group, const Task &task, int error,
      const common::ObString &outcome, int64_t max_retries);
  common::ObMySQLProxy *sql_;
  char owner_buffer_[64];
  char prefix_buffer_[24];
  common::ObString owner_;
  common::ObString prefix_;
  std::atomic<bool> cancellation_;
  std::atomic<int> reclaimed_;
  bool started_;
  DISALLOW_COPY_AND_ASSIGN(AIColumnWorker);
};
} }
