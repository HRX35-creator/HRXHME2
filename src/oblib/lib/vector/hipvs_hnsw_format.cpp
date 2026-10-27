/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "hipvs_hnsw_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <vector>

namespace oceanbase {
namespace common {
namespace obvsag {
namespace {
constexpr size_t HEADER_SIZE = 96;

uint64_t read_u64(const uint8_t *p)
{
  uint64_t value = 0;
  for (unsigned int i = 0; i < 8; ++i) { value |= uint64_t(p[i]) << (8 * i); }
  return value;
}

uint32_t read_u32(const uint8_t *p)
{
  uint32_t value = 0;
  for (unsigned int i = 0; i < 4; ++i) { value |= uint32_t(p[i]) << (8 * i); }
  return value;
}

void write_u64(uint8_t *p, uint64_t value)
{
  for (unsigned int i = 0; i < 8; ++i) { p[i] = uint8_t(value >> (8 * i)); }
}
}  // namespace

bool prepare_hipvs_hnsw_blob(uint8_t *bytes, size_t length,
                            const HipvsHnswSource &source, std::string &error)
{
  error.clear();
  const auto reject = [&error](const char *message) { error = message; return false; };
  const uint16_t endian = 1;
  if (sizeof(size_t) != 8 || sizeof(float) != 4 || !std::numeric_limits<float>::is_iec559
      || *reinterpret_cast<const uint8_t *>(&endian) != 1) {
    return reject("hipVS HNSW import requires a little-endian 64-bit IEEE float32 host");
  }
  if (bytes == nullptr || length < HEADER_SIZE || source.vectors == nullptr || source.ids == nullptr
      || source.rows == 0 || source.rows > UINT32_MAX || source.dimension == 0
      || source.max_degree < 2 || source.max_degree > 32767 || source.ef_construction == 0) {
    return reject("invalid hipVS HNSW source or truncated header");
  }
  const uint64_t rows = read_u64(bytes + 16);
  const uint64_t stride = read_u64(bytes + 24);
  const uint64_t label_offset = read_u64(bytes + 32);
  const uint64_t vector_offset = read_u64(bytes + 40);
  const uint32_t max_level = read_u32(bytes + 48);
  const uint32_t entrypoint = read_u32(bytes + 52);
  const uint64_t m = source.max_degree;
  const uint64_t m0 = 2 * m;
  const uint64_t vector_bytes = uint64_t(source.dimension) * sizeof(float);
  double multiplier = 0;
  std::memcpy(&multiplier, bytes + 80, sizeof(multiplier));
  if (read_u64(bytes) != 0 || rows != source.rows || read_u64(bytes + 8) < rows
      || vector_offset != sizeof(uint32_t) + m0 * sizeof(uint32_t)
      || label_offset != vector_offset + vector_bytes || stride != label_offset + sizeof(int64_t)
      || read_u64(bytes + 56) != m || read_u64(bytes + 64) != m0 || read_u64(bytes + 72) != m
      || read_u64(bytes + 88) != std::max<uint64_t>(m, source.ef_construction)
      || max_level > 63 || entrypoint >= rows || !std::isfinite(multiplier)
      || std::abs(multiplier - 1.0 / std::log(double(m))) > 1e-12) {
    return reject("hipVS HNSW shape, hierarchy or construction parameters differ from the source");
  }
  if (rows > (length - HEADER_SIZE) / stride) {
    return reject("truncated hipVS HNSW base layer");
  }
  const size_t base_end = HEADER_SIZE + size_t(rows * stride);
  if (rows > (length - base_end) / sizeof(uint32_t)) {
    return reject("truncated hipVS HNSW upper-layer lengths");
  }
  std::vector<uint64_t> ordinals(size_t(rows), 0);
  std::vector<uint8_t> seen_ordinals(size_t(rows), 0);
  std::vector<uint64_t> neighbor_marks(size_t(rows), 0);
  std::vector<uint32_t> levels(size_t(rows), 0);
  std::unordered_set<int64_t> external_ids;
  external_ids.reserve(size_t(rows));
  for (uint64_t i = 0; i < rows; ++i) {
    if (!external_ids.insert(source.ids[i]).second) {
      return reject("source VIDs are not unique");
    }
  }
  uint64_t mark = 0;
  const auto valid_links = [&](const uint8_t *block, uint32_t capacity, uint32_t self) {
    const uint32_t count = read_u32(block);
    if (count > capacity) { return false; }  // also rejects deleted-node flags
    ++mark;
    for (uint32_t j = 0; j < count; ++j) {
      const uint32_t neighbor = read_u32(block + sizeof(uint32_t) * (j + 1));
      if (neighbor >= rows || neighbor == self || neighbor_marks[neighbor] == mark) { return false; }
      neighbor_marks[neighbor] = mark;
    }
    return true;
  };
  for (uint32_t i = 0; i < rows; ++i) {
    const uint8_t *record = bytes + HEADER_SIZE + size_t(i) * stride;
    const uint64_t ordinal = read_u64(record + label_offset);
    if (ordinal >= rows || seen_ordinals[ordinal] != 0) {
      return reject("hipVS HNSW contains missing, duplicated or invalid source ordinals");
    }
    ordinals[i] = ordinal;
    seen_ordinals[ordinal] = 1;
    if (std::memcmp(record + vector_offset, source.vectors + ordinal * source.dimension, size_t(vector_bytes)) != 0) {
      return reject("hipVS HNSW vector differs from its original float32 source");
    }
    if (!valid_links(record, uint32_t(m0), i)) {
      return reject("hipVS HNSW contains an invalid base-layer edge or deletion flag");
    }
  }
  size_t cursor = base_end;
  uint32_t observed_max_level = 0;
  const uint64_t upper_stride = sizeof(uint32_t) + m * sizeof(uint32_t);
  for (uint32_t i = 0; i < rows; ++i) {
    if (length - cursor < sizeof(uint32_t)) { return reject("truncated hipVS HNSW hierarchy"); }
    const uint32_t upper_bytes = read_u32(bytes + cursor);
    cursor += sizeof(uint32_t);
    if (upper_bytes % upper_stride != 0 || upper_bytes / upper_stride > max_level || upper_bytes > length - cursor) {
      return reject("invalid hipVS HNSW upper-layer block size");
    }
    levels[i] = uint32_t(upper_bytes / upper_stride);
    observed_max_level = std::max(observed_max_level, levels[i]);
    for (uint32_t level = 0; level < levels[i]; ++level) {
      if (!valid_links(bytes + cursor + level * upper_stride, uint32_t(m), i)) {
        return reject("hipVS HNSW contains an invalid upper-layer edge");
      }
    }
    cursor += upper_bytes;
  }
  if (cursor != length || observed_max_level != max_level || levels[entrypoint] != max_level) {
    return reject("hipVS HNSW has trailing data or an inconsistent entrypoint hierarchy");
  }
  cursor = base_end;
  for (uint32_t i = 0; i < rows; ++i) {
    const uint32_t upper_bytes = read_u32(bytes + cursor);
    cursor += sizeof(uint32_t);
    for (uint32_t level = 1; level <= levels[i]; ++level) {
      const uint8_t *block = bytes + cursor + (level - 1) * upper_stride;
      for (uint32_t j = 0; j < read_u32(block); ++j) {
        if (levels[read_u32(block + (j + 1) * sizeof(uint32_t))] < level) {
          return reject("hipVS HNSW edge targets a node without that upper layer");
        }
      }
    }
    cursor += upper_bytes;
  }
  // VSAG's mutable HNSW accepts this full hnswlib layout. Rewrite stored labels,
  // not internal row positions: parallel hierarchy construction can reorder nodes.
  write_u64(bytes + 8, rows);
  for (uint64_t i = 0; i < rows; ++i) {
    write_u64(bytes + HEADER_SIZE + size_t(i) * stride + label_offset,
              static_cast<uint64_t>(source.ids[ordinals[i]]));
  }
  return true;
}

}  // namespace obvsag
}  // namespace common
}  // namespace oceanbase
