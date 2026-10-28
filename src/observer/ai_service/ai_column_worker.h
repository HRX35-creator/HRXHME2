/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "share/ob_thread_pool.h"

namespace oceanbase
{
namespace observer
{
// Claims committed request groups, performs inference without holding row locks,
// and publishes the entire group in one transaction. Leases survive a restart.
class AiColumnWorker final : public share::ObThreadPool
{
public:
  AiColumnWorker() : share::ObThreadPool(4) {}
  void run1() override;
private:
  int process_one();
  int enqueue_missing();
};
} // namespace observer
} // namespace oceanbase
