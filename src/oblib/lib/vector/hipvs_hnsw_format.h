/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace oceanbase {
namespace common {
namespace obvsag {

struct HipvsHnswSource {
  const float *vectors;
  const int64_t *ids;
  uint64_t rows;
  uint32_t dimension;
  uint32_t max_degree;
  uint32_t ef_construction;
};

// Accept the full, mutable hnswlib format produced by hipVS hierarchy=CPU.
// Validate every vector/ordinal/edge before replacing ordinal labels with VIDs.
// On validation failure, the caller's buffer is unchanged. Throws on allocation failure.
bool prepare_hipvs_hnsw_blob(uint8_t *bytes, size_t length,
                            const HipvsHnswSource &source, std::string &error);

}  // namespace obvsag
}  // namespace common
}  // namespace oceanbase
