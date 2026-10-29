/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace oceanbase::common::hipvs {

// The experimental CAGRA HNSW export is not an ordinary mutable HNSW index:
// it advertises level 1 with no upper links, uses ordinal labels and an arbitrary
// level multiplier. Validate the complete export BEFORE adapting it to VSAG.
// Contract: AMD-Ecosystem/hipVS f96a663d, cagra_serialize.cuh,
// float32 vectors / uint32 graph / little-endian LP64 only.
// This conversion does not build or repair a graph, nor establish its recall.
class GraphImport {
public:
  static constexpr size_t HEADER_BYTES = 96;

  // nullptr denotes success. Failure leaves bytes unchanged. bad_alloc is
  // propagated to the caller's existing allocation/error boundary.
  static const char *convert(std::span<unsigned char> bytes,
                             std::span<const float> vectors,
                             std::span<const int64_t> ids,
                             uint32_t dim, uint32_t m, uint64_t ef_construction)
  {
    const uint16_t endian = 1;
    if (sizeof(size_t) != 8 || sizeof(float) != 4 || sizeof(double) != 8 ||
        *reinterpret_cast<const unsigned char *>(&endian) != 1 ||
        !std::numeric_limits<float>::is_iec559) {
      return "unsupported graph serialization ABI";
    }
    const uint64_t n = ids.size();
    if (dim == 0 || m < 2 || m > 128 || n <= 2 * m ||
        n >= std::numeric_limits<uint32_t>::max() || ef_construction <= m) {
      return "unsupported graph dimensions, degree or construction parameters";
    }
    const uint64_t degree = 2 * m;
    const uint64_t vector_offset = 4 + degree * 4;
    const uint64_t label_offset = vector_offset + uint64_t(dim) * 4;
    const uint64_t row_bytes = label_offset + 8;
    if (n > (std::numeric_limits<size_t>::max() - HEADER_BYTES) / (row_bytes + 4) ||
        n > std::numeric_limits<size_t>::max() / dim ||
        vectors.size() != n * dim || bytes.size() != HEADER_BYTES + n * (row_bytes + 4)) {
      return "graph size mismatch or overflow";
    }
    if (read<uint64_t>(bytes, 0) != 0 || read<uint64_t>(bytes, 8) != n ||
        read<uint64_t>(bytes, 16) != n || read<uint64_t>(bytes, 24) != row_bytes ||
        read<uint64_t>(bytes, 32) != label_offset || read<uint64_t>(bytes, 40) != vector_offset ||
        read<int32_t>(bytes, 48) != 1 || read<uint32_t>(bytes, 52) != n / 2 ||
        read<uint64_t>(bytes, 56) != m || read<uint64_t>(bytes, 64) != degree ||
        read<uint64_t>(bytes, 72) != m || read<double>(bytes, 80) != 0.42424242 ||
        read<uint64_t>(bytes, 88) != 500) {
      return "unrecognized CAGRA export header";
    }
    // Internal ordinals must have a one-to-one mapping to the caller's vids.
    std::vector<int64_t> sorted_ids(ids.begin(), ids.end());
    std::sort(sorted_ids.begin(), sorted_ids.end());
    if (std::adjacent_find(sorted_ids.begin(), sorted_ids.end()) != sorted_ids.end()) {
      return "duplicate vector ID";
    }
    std::array<uint32_t, 256> neighbors;
    for (uint64_t row = 0; row < n; ++row) {
      const size_t offset = HEADER_BYTES + row * row_bytes;
      if (read<uint32_t>(bytes, offset) != degree ||
          read<uint64_t>(bytes, offset + label_offset) != row ||
          read<uint32_t>(bytes, HEADER_BYTES + n * row_bytes + row * 4) != 0) {
        return "invalid graph row, ordinal label or upper layer";
      }
      for (uint32_t j = 0; j < degree; ++j) {
        neighbors[j] = read<uint32_t>(bytes, offset + 4 + j * 4);
        if (neighbors[j] >= n || neighbors[j] == row) {
          return "invalid graph neighbor";
        }
      }
      std::sort(neighbors.begin(), neighbors.begin() + degree);
      if (std::adjacent_find(neighbors.begin(), neighbors.begin() + degree) != neighbors.begin() + degree) {
        return "duplicate graph neighbor";
      }
      if (std::memcmp(bytes.data() + offset + vector_offset,
                      vectors.data() + row * dim, size_t(dim) * sizeof(float)) != 0) {
        return "exported vectors differ from input order or values";
      }
      for (uint32_t d = 0; d < dim; ++d) {
        if (!std::isfinite(vectors[row * dim + d])) {
          return "non-finite input vector";
        }
      }
    }
    // No fallible operations after this point: failed validation is atomic.
    write<int32_t>(bytes, 48, 0);
    write<double>(bytes, 80, 1.0 / std::log(double(m)));
    write<uint64_t>(bytes, 88, ef_construction);
    for (uint64_t row = 0; row < n; ++row) {
      write<int64_t>(bytes, HEADER_BYTES + row * row_bytes + label_offset, ids[row]);
    }
    return nullptr;
  }

private:
  template <class T> static T read(std::span<const unsigned char> bytes, size_t offset)
  {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
  }
  template <class T> static void write(std::span<unsigned char> bytes, size_t offset, T value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
  }
};

} // namespace oceanbase::common::hipvs
