/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "hipvs_hnsw_format.h"
#include "vsag/binaryset.h"

namespace oceanbase {
namespace common {
namespace obvsag {

enum class HipvsBuildMode { DISABLED, PREFER, REQUIRED, INVALID };
HipvsBuildMode hipvs_build_mode();

struct HipvsBuildReport {
  std::string error;
  std::string device;
  std::string architecture;
  uint64_t bytes = 0;
  double cagra_build_ms = 0;
  double hierarchy_serialize_ms = 0;
  uint32_t nn_descent_iterations = 0;
};

// Returns an unpublished, validated HNSW BinarySet. No existing index is modified.
bool build_hipvs_hnsw(const HipvsHnswSource &source, uint32_t cpu_threads,
                      vsag::BinarySet &output, HipvsBuildReport &report);

}  // namespace obvsag
}  // namespace common
}  // namespace oceanbase
