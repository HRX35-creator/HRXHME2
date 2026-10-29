/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "hipvs_graph_topology.h"
#include <iostream>
#include <stdexcept>

void check(bool condition)
{
  if (!condition) throw std::runtime_error("graph topology regression failed");
}

int main() try
{
  using hipvs_diagnostic::analyze_graph;
  using Graph = std::vector<std::vector<uint32_t>>;
  // All vertices have incoming edges and are reachable from 0, but a search
  // entering the second pair cannot return. Forward-only coverage misses this.
  const Graph one_way{{1}, {0, 2}, {3}, {2}};
  const auto original = one_way;
  auto result = analyze_graph(one_way, 0);
  check(result.reachable_from_entry == 4 && result.zero_indegree == 0);
  check(result.can_reach_entry == 2 && result.entry_strong_component == 2);
  check(result.edges == 5 && result.reciprocal_edges == 4);
  check(result.max_indegree == 2 && result.p50_indegree == 1 && result.p95_indegree == 2);
  check(one_way == original);
  result = analyze_graph(one_way, 2);
  check(result.reachable_from_entry == 2 && result.can_reach_entry == 4);
  check(result.entry_strong_component == 2);

  result = analyze_graph(Graph{{1}, {0}, {3}, {2}}, 0);
  check(result.reachable_from_entry == 2 && result.can_reach_entry == 2);
  check(result.zero_indegree == 0 && result.reciprocal_edges == 4);
  result = analyze_graph(Graph{{1}, {2}, {0}}, 0);
  check(result.entry_strong_component == 3 && result.reciprocal_edges == 0);
  result = analyze_graph(Graph{{1, 2}, {}, {}}, 0);
  check(result.reachable_from_entry == 3 && result.entry_strong_component == 1);
  check(result.zero_indegree == 1);
  result = analyze_graph(Graph{{}}, 0);
  check(result.edges == 0 && result.entry_strong_component == 1 && result.max_indegree == 0);

  for (const auto &bad : std::vector<Graph>{{}, {{0}}, {{1}}, {{1, 1}, {0}}}) {
    bool rejected = false;
    try { analyze_graph(bad, 0); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
  }
  bool rejected = false;
  try { analyze_graph(Graph{{}}, 1); } catch (const std::invalid_argument &) { rejected = true; }
  check(rejected);
  std::cout << "PASS: directed connectivity, reciprocal edges, degree distribution and invalid graphs\n";
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
