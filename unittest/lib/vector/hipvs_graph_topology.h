/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hipvs_diagnostic {

struct GraphTopology {
  uint64_t edges = 0;
  uint64_t reachable_from_entry = 0;
  uint64_t can_reach_entry = 0;
  uint64_t entry_strong_component = 0;
  uint64_t reciprocal_edges = 0;
  uint64_t zero_indegree = 0;
  uint64_t max_indegree = 0;
  uint64_t p50_indegree = 0;
  uint64_t p95_indegree = 0;
};

// Read-only diagnostic. It never repairs edges or changes the search entry.
inline GraphTopology analyze_graph(const std::vector<std::vector<uint32_t>> &graph,
                                   uint32_t entry)
{
  if (graph.empty() || graph.size() > UINT32_MAX || entry >= graph.size()) {
    throw std::invalid_argument("invalid graph entry or cardinality");
  }
  auto sorted = graph;
  std::vector<std::vector<uint32_t>> reverse(graph.size());
  GraphTopology result;
  for (size_t row = 0; row < graph.size(); ++row) {
    auto &neighbors = sorted[row];
    std::sort(neighbors.begin(), neighbors.end());
    if (std::adjacent_find(neighbors.begin(), neighbors.end()) != neighbors.end()) {
      throw std::invalid_argument("duplicate graph edge");
    }
    for (uint32_t neighbor : neighbors) {
      if (neighbor >= graph.size() || neighbor == row) {
        throw std::invalid_argument("invalid graph edge");
      }
      reverse[neighbor].push_back(uint32_t(row));
      ++result.edges;
    }
  }
  auto reachable = [entry](const auto &edges) {
    std::vector<bool> seen(edges.size(), false);
    std::vector<uint32_t> queue{entry};
    seen[entry] = true;
    for (size_t pos = 0; pos < queue.size(); ++pos) {
      for (uint32_t neighbor : edges[queue[pos]]) {
        if (!seen[neighbor]) {
          seen[neighbor] = true;
          queue.push_back(neighbor);
        }
      }
    }
    return seen;
  };
  const auto forward_seen = reachable(graph);
  const auto reverse_seen = reachable(reverse);
  std::vector<uint64_t> indegrees;
  indegrees.reserve(graph.size());
  for (size_t row = 0; row < graph.size(); ++row) {
    result.reachable_from_entry += forward_seen[row];
    result.can_reach_entry += reverse_seen[row];
    result.entry_strong_component += forward_seen[row] && reverse_seen[row];
    indegrees.push_back(reverse[row].size());
    result.zero_indegree += reverse[row].empty();
    for (uint32_t neighbor : graph[row]) {
      result.reciprocal_edges += std::binary_search(sorted[neighbor].begin(),
                                                   sorted[neighbor].end(), uint32_t(row));
    }
  }
  std::sort(indegrees.begin(), indegrees.end());
  result.max_indegree = indegrees.back();
  // Nearest-rank percentiles, including graphs containing just one node.
  result.p50_indegree = indegrees[(indegrees.size() * 50 + 99) / 100 - 1];
  result.p95_indegree = indegrees[(indegrees.size() * 95 + 99) / 100 - 1];
  return result;
}

} // namespace hipvs_diagnostic
