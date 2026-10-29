/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace oceanbase::common::hipvs {
// Refine a GraphImport-validated flat graph using outgoing and reverse edges.
// Keep its original BFS tree, so every originally reachable node remains so.
// The input is unchanged on validation/allocation failure. Only neighbor IDs
// change on success; no query, label, external ID or answer affects selection.
class GraphRefine {
public:
  static const char *refine(std::span<unsigned char> bytes,
                            std::span<const float> vectors, uint32_t dim, uint32_t m)
  {
    if (bytes.size() < 96 || dim == 0 || m < 2 || m > 128 || vectors.size() % dim)
      return "invalid refinement dimensions";
    const uint64_t n = vectors.size() / dim, degree = 2 * uint64_t(m);
    const uint64_t vector_offset = 4 + degree * 4;
    const uint64_t label_offset = vector_offset + uint64_t(dim) * 4;
    const uint64_t stride = label_offset + 8;
    if (n <= degree || n >= UINT32_MAX || n > (SIZE_MAX - 96) / (stride + 4) ||
        n > SIZE_MAX / degree || bytes.size() != 96 + n * (stride + 4) ||
        read<uint64_t>(bytes, 16) != n || read<uint64_t>(bytes, 24) != stride ||
        read<uint64_t>(bytes, 32) != label_offset || read<uint64_t>(bytes, 40) != vector_offset ||
        read<int32_t>(bytes, 48) != 0 || read<uint32_t>(bytes, 52) >= n)
      return "invalid flat graph layout";
    for (float v : vectors) if (!std::isfinite(v)) return "non-finite refinement vector";
    const auto edge = [&](uint32_t u, uint32_t j) {
      return read<uint32_t>(bytes, 96 + uint64_t(u) * stride + 4 + uint64_t(j) * 4);
    };
    std::vector<size_t> offsets(n + 1, 0);
    std::array<uint32_t, 256> sorted;
    for (uint32_t u = 0; u < n; ++u) {
      const auto offset = 96 + uint64_t(u) * stride;
      if (read<uint32_t>(bytes, offset) != degree ||
          std::memcmp(bytes.data() + offset + vector_offset, vectors.data() + uint64_t(u) * dim, size_t(dim) * 4))
        return "invalid refinement row or vector";
      for (uint32_t j = 0; j < degree; ++j) {
        const auto v = edge(u, j);
        if (v >= n || v == u) return "invalid refinement neighbor";
        sorted[j] = v; ++offsets[v + 1];
      }
      std::sort(sorted.begin(), sorted.begin() + degree);
      if (std::adjacent_find(sorted.begin(), sorted.begin() + degree) != sorted.begin() + degree)
        return "duplicate refinement neighbor";
    }
    for (size_t i = 1; i <= n; ++i) offsets[i] += offsets[i - 1];
    std::vector<uint32_t> reverse(n * degree), output(n * degree), parent(n, UINT32_MAX), queue;
    auto cursor = offsets;
    for (uint32_t u = 0; u < n; ++u)
      for (uint32_t j = 0; j < degree; ++j) reverse[cursor[edge(u, j)]++] = u;
    const auto entry = read<uint32_t>(bytes, 52);
    parent[entry] = entry; queue.reserve(n); queue.push_back(entry);
    for (size_t p = 0; p < queue.size(); ++p) {
      const auto u = queue[p];
      for (uint32_t j = 0; j < degree; ++j) {
        const auto v = edge(u, j);
        if (parent[v] == UINT32_MAX) { parent[v] = u; queue.push_back(v); }
      }
    }
    std::vector<uint32_t> candidates, selected;
    std::vector<std::pair<double, uint32_t>> ranked;
    selected.reserve(degree);
    for (uint32_t u = 0; u < n; ++u) {
      candidates.clear(); selected.clear(); ranked.clear();
      for (uint32_t j = 0; j < degree; ++j) {
        const auto v = edge(u, j); candidates.push_back(v);
        if (parent[v] == u) selected.push_back(v);
      }
      candidates.insert(candidates.end(), reverse.begin() + offsets[u], reverse.begin() + offsets[u + 1]);
      std::sort(candidates.begin(), candidates.end());
      candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
      for (auto v : candidates) {
        double distance = 0;
        for (uint32_t d = 0; d < dim; ++d) {
          const double delta = double(vectors[uint64_t(u) * dim + d]) - double(vectors[uint64_t(v) * dim + d]);
          distance += delta * delta;
        }
        ranked.emplace_back(distance, v);
      }
      std::sort(ranked.begin(), ranked.end());
      for (const auto &candidate : ranked) {
        if (selected.size() == degree) break;
        if (std::find(selected.begin(), selected.end(), candidate.second) == selected.end())
          selected.push_back(candidate.second);
      }
      if (selected.size() != degree) return "insufficient refinement candidates";
      std::copy(selected.begin(), selected.end(), output.begin() + uint64_t(u) * degree);
    }
    // All allocation and validation is complete before the first mutation.
    for (uint32_t u = 0; u < n; ++u)
      std::memcpy(bytes.data() + 96 + uint64_t(u) * stride + 4, output.data() + uint64_t(u) * degree, degree * 4);
    return nullptr;
  }
private:
  template<class T> static T read(std::span<const unsigned char> bytes, size_t offset)
  {
    T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
  }
};
} // namespace oceanbase::common::hipvs
