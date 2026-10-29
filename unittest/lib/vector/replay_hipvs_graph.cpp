/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Replay a captured REAL hipVS export against the pinned VSAG consumer, without
// a GPU. Diagnostic timings are not end-to-end SQL construction measurements.
#include "lib/vector/hipvs_graph_import.h"
#include "hipvs_graph_topology.h"
#include <vsag/vsag.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>

using Bytes = std::vector<unsigned char>;
using Clock = std::chrono::steady_clock;

template<class T> T take(tl::expected<T, vsag::Error> result)
{
  if (!result) throw std::runtime_error(result.error().message);
  return result.value();
}
void take(tl::expected<void, vsag::Error> result)
{
  if (!result) throw std::runtime_error(result.error().message);
}
template<class T> T read(const Bytes &b, size_t offset)
{
  if (offset > b.size() || sizeof(T) > b.size() - offset) throw std::runtime_error("short header");
  T value;
  std::memcpy(&value, b.data() + offset, sizeof(value));
  return value;
}
template<class T> std::vector<T> file(const char *path, size_t count = 0)
{
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0) throw std::runtime_error("cannot open input");
  const auto bytes = size_t(f.tellg());
  if (bytes % sizeof(T) || (count && count > bytes / sizeof(T))) throw std::runtime_error("input size mismatch");
  std::vector<T> values(count ? count : bytes / sizeof(T));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(values.data()), values.size() * sizeof(T))) throw std::runtime_error("short input");
  return values;
}

hipvs_diagnostic::GraphTopology topology(const Bytes &bytes)
{
  if (bytes.size() < 96) throw std::runtime_error("short topology header");
  const uint64_t n = read<uint64_t>(bytes, 16), stride = read<uint64_t>(bytes, 24);
  const uint64_t vector_offset = read<uint64_t>(bytes, 40);
  if (n == 0 || n > UINT32_MAX || vector_offset < 4 || vector_offset > stride ||
      stride > bytes.size() || n > (bytes.size() - 96) / stride) {
    throw std::runtime_error("invalid topology serialization dimensions");
  }
  std::vector<std::vector<uint32_t>> graph(n);
  for (uint64_t row = 0; row < n; ++row) {
    const size_t offset = 96 + row * stride;
    const uint32_t count = read<uint32_t>(bytes, offset);
    if (uint64_t(count) * 4 > vector_offset - 4) throw std::runtime_error("invalid topology degree");
    for (uint32_t j = 0; j < count; ++j) graph[row].push_back(read<uint32_t>(bytes, offset + 4 + 4 * j));
  }
  return hipvs_diagnostic::analyze_graph(graph, read<uint32_t>(bytes, 52));
}

int main(int argc, char **argv) try
{
  if (argc != 5) throw std::runtime_error("usage: replay_hipvs_graph export.bin|--cpu-control base.f32 query.f32 report.json (CPU control uses dim=128,m=16)");
  const bool cpu_control = std::string(argv[1]) == "--cpu-control";
  Bytes raw = cpu_control ? Bytes{} : file<unsigned char>(argv[1]);
  if (cpu_control && std::filesystem::file_size(argv[2]) % (128 * sizeof(float))) throw std::runtime_error("CPU control requires dim=128");
  const uint64_t n = cpu_control ? std::filesystem::file_size(argv[2]) / (128 * sizeof(float)) : read<uint64_t>(raw, 16);
  const uint64_t m = cpu_control ? 16 : read<uint64_t>(raw, 72);
  const uint64_t label_offset = cpu_control ? 644 : read<uint64_t>(raw, 32), vector_offset = cpu_control ? 132 : read<uint64_t>(raw, 40);
  if (label_offset <= vector_offset || (label_offset - vector_offset) % 4 || n > UINT32_MAX) throw std::runtime_error("invalid dimensions");
  const uint64_t dim = (label_offset - vector_offset) / 4, stride = cpu_control ? 652 : read<uint64_t>(raw, 24);
  if (n <= 2 * m || dim > 65536 || n > SIZE_MAX / dim || m < 2 || m > 128) throw std::runtime_error("invalid shape");
  auto base = file<float>(argv[2], n * dim);
  auto queries = file<float>(argv[3]);
  if (queries.empty() || queries.size() % dim) throw std::runtime_error("invalid queries");
  for (float v : queries) if (!std::isfinite(v)) throw std::runtime_error("non-finite query");
  const size_t nq = queries.size() / dim, k = 10;
  std::vector<int64_t> ids(n);
  std::iota(ids.begin(), ids.end(), 0);
  if (!cpu_control)
    if (const char *error = oceanbase::common::hipvs::GraphImport::convert(raw, base, ids, dim, m, 200)) throw std::runtime_error(error);
  // The converter's contract preserves the complete graph and vector payload.
  const uint32_t original_entry = cpu_control ? 0 : read<uint32_t>(raw, 52);
  std::vector<uint32_t> incoming(n, 0), queue;
  std::vector<bool> seen(n, false);
  for (uint64_t row = 0; !cpu_control && row < n; ++row)
    for (uint64_t j = 0; j < 2 * m; ++j)
      ++incoming[read<uint32_t>(raw, 96 + row * stride + 4 + 4 * j)];
  seen[original_entry] = true;
  queue.push_back(original_entry);
  for (size_t pos = 0; !cpu_control && pos < queue.size(); ++pos) {
    for (uint64_t j = 0; j < 2 * m; ++j) {
      const auto v = read<uint32_t>(raw, 96 + queue[pos] * stride + 4 + 4 * j);
      if (!seen[v]) { seen[v] = true; queue.push_back(v); }
    }
  }
  std::vector<double> mean(dim, 0);
  for (uint64_t row = 0; row < n; ++row)
    for (uint64_t j = 0; j < dim; ++j) mean[j] += double(base[row * dim + j]) / double(n);
  uint32_t centroid_entry = 0;
  double best = std::numeric_limits<double>::infinity();
  for (uint64_t row = 0; row < n; ++row) {
    double distance = 0;
    for (uint64_t j = 0; j < dim; ++j) {
      const double delta = double(base[row * dim + j]) - mean[j];
      distance += delta * delta;
    }
    if (distance < best) { best = distance; centroid_entry = row; }
  }
  const uint32_t indegree_entry = std::max_element(incoming.begin(), incoming.end()) - incoming.begin();
  std::vector<std::vector<int64_t>> truth(nq);
  for (size_t q = 0; q < nq; ++q) {
    std::vector<std::pair<double, int64_t>> distances(n);
    for (uint64_t row = 0; row < n; ++row) {
      double distance = 0;
      for (uint64_t j = 0; j < dim; ++j) {
        const double delta = double(base[row * dim + j]) - double(queries[q * dim + j]);
        distance += delta * delta;
      }
      distances[row] = {distance, int64_t(row)};
    }
    std::partial_sort(distances.begin(), distances.begin() + k, distances.end());
    for (size_t rank = 0; rank < k; ++rank) truth[q].push_back(distances[rank].second);
  }
  const std::string parameters = "{\"dtype\":\"float32\",\"metric_type\":\"l2\",\"dim\":" + std::to_string(dim) +
    ",\"use_old_serial_format\":true,\"hnsw\":{\"max_degree\":" + std::to_string(m) + ",\"ef_construction\":200,\"ef_search\":64}}";
  std::ofstream report(argv[4]);
  if (!report) throw std::runtime_error("cannot write report");
  report << std::setprecision(12) << "{\"scope\":\"" << (cpu_control ? "CPU hierarchy ablation; no GPU graph involved" : "CPU replay of captured GPU graph; not SQL build benchmark") << "\",\"rows\":" << n
    << ",\"dim\":" << dim << ",\"queries\":" << nq << ",\"m\":" << m
    << ",\"reachable_from_original_entry\":" << (cpu_control ? "null" : std::to_string(queue.size())) << ",\"zero_indegree\":" << (cpu_control ? "null" : std::to_string(std::count(incoming.begin(), incoming.end(), 0)))
    << ",\"original_entry\":" << (cpu_control ? "null" : std::to_string(original_entry)) << ",\"centroid_entry\":" << centroid_entry << ",\"max_indegree_entry\":" << (cpu_control ? "null" : std::to_string(indegree_entry)) << ",\"runs\":[";
  bool first = true;
  Bytes cpu_flat;
  for (int variant = cpu_control ? 3 : 0; variant < 6; ++variant) {
    auto index = take(vsag::Factory::CreateIndex("hnsw", parameters));
    const auto began = Clock::now();
    const char *name = "cpu_hnsw";
    const Bytes *topology_bytes = &raw;
    if (variant == 3) {
      auto data = vsag::Dataset::Make()->NumElements(n)->Dim(dim)->Ids(ids.data())->Float32Vectors(base.data())->Owner(false);
      if (!take(index->Build(data)).empty()) throw std::runtime_error("CPU build rejected IDs");
      // Remove ONLY upper links from this same CPU-built graph. Keep its exact
      // base edges/vectors/labels and original entry to isolate hierarchy.
      const auto binary = take(index->Serialize());
      const auto blob = binary.Get(vsag::HNSW_DATA);
      cpu_flat.assign(reinterpret_cast<unsigned char *>(blob.data.get()), reinterpret_cast<unsigned char *>(blob.data.get()) + blob.size);
      topology_bytes = &cpu_flat;
      if (read<uint64_t>(cpu_flat, 16) != n || read<uint64_t>(cpu_flat, 24) != stride ||
          read<uint64_t>(cpu_flat, 32) != label_offset || read<uint64_t>(cpu_flat, 40) != vector_offset ||
          read<uint32_t>(cpu_flat, 52) >= n || cpu_flat.size() < 96 + n * (stride + 4)) throw std::runtime_error("unexpected CPU graph format");
      const int32_t level = 0;
      std::memcpy(cpu_flat.data() + 48, &level, sizeof(level));
      cpu_flat.resize(96 + n * stride);
      cpu_flat.resize(96 + n * (stride + 4), 0);
    } else if (variant >= 4) {
      name = variant == 4 ? "cpu_flat_original_entry" : "cpu_flat_centroid_entry";
      if (variant == 5) std::memcpy(cpu_flat.data() + 52, &centroid_entry, sizeof(centroid_entry));
      vsag::Binary blob;
      blob.size = cpu_flat.size();
      blob.data = std::shared_ptr<int8_t[]>(reinterpret_cast<int8_t *>(cpu_flat.data()), [](int8_t *) {});
      vsag::BinarySet binary;
      binary.Set(vsag::HNSW_DATA, blob);
      take(index->Deserialize(binary));
      topology_bytes = &cpu_flat;
    } else {
      const uint32_t entry = variant == 0 ? original_entry : variant == 1 ? centroid_entry : indegree_entry;
      name = variant == 0 ? "gpu_original_entry" : variant == 1 ? "gpu_centroid_entry" : "gpu_max_indegree_entry";
      std::memcpy(raw.data() + 52, &entry, sizeof(entry));
      vsag::Binary blob;
      blob.size = raw.size();
      blob.data = std::shared_ptr<int8_t[]>(reinterpret_cast<int8_t *>(raw.data()), [](int8_t *) {});
      vsag::BinarySet binary;
      binary.Set(vsag::HNSW_DATA, blob);
      take(index->Deserialize(binary));
    }
    if (uint64_t(index->GetNumElements()) != n) throw std::runtime_error("cardinality changed");
    const double load_seconds = std::chrono::duration<double>(Clock::now() - began).count();
    const auto shape = topology(*topology_bytes);
    for (int ef : {64, 128, 256, 512}) {
      if (!first) report << ',';
      first = false;
      report << "{\"variant\":\"" << name << "\",\"ef_search\":" << ef << ",\"load_or_cpu_build_seconds\":" << load_seconds
        << ",\"topology\":{\"edges\":" << shape.edges
        << ",\"reachable_from_entry\":" << shape.reachable_from_entry
        << ",\"can_reach_entry\":" << shape.can_reach_entry
        << ",\"entry_strong_component\":" << shape.entry_strong_component
        << ",\"reciprocal_edges\":" << shape.reciprocal_edges
        << ",\"zero_indegree\":" << shape.zero_indegree
        << ",\"max_indegree\":" << shape.max_indegree
        << ",\"p50_indegree\":" << shape.p50_indegree
        << ",\"p95_indegree\":" << shape.p95_indegree << "},\"query_results\":[";
      size_t hits = 0;
      double query_seconds = 0;
      for (size_t q = 0; q < nq; ++q) {
        auto query = vsag::Dataset::Make()->NumElements(1)->Dim(dim)->Float32Vectors(queries.data() + q * dim)->Owner(false);
        const auto started = Clock::now();
        auto result = take(index->KnnSearch(query, k, "{\"hnsw\":{\"ef_search\":" + std::to_string(ef) + "}}"));
        query_seconds += std::chrono::duration<double>(Clock::now() - started).count();
        if (result->GetDim() != k) throw std::runtime_error("top-k count mismatch");
        std::vector<int64_t> returned(result->GetIds(), result->GetIds() + k);
        auto sorted = returned;
        std::sort(sorted.begin(), sorted.end());
        if (sorted.front() < 0 || uint64_t(sorted.back()) >= n || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) throw std::runtime_error("invalid returned ID");
        size_t found = 0;
        for (auto id : returned) found += std::find(truth[q].begin(), truth[q].end(), id) != truth[q].end();
        for (size_t rank = 0; rank < k; ++rank) {
          double distance = 0;
          for (uint64_t j = 0; j < dim; ++j) {
            const double delta = double(base[returned[rank] * dim + j]) - double(queries[q * dim + j]);
            distance += delta * delta;
          }
          if (!std::isfinite(result->GetDistances()[rank]) ||
              std::abs(double(result->GetDistances()[rank]) - distance) > 1e-4 * (1 + distance)) throw std::runtime_error("returned distance disagrees with vector ID");
        }
        hits += found;
        if (q) report << ',';
        report << "{\"hits\":" << found << ",\"ids\":[";
        for (size_t rank = 0; rank < k; ++rank) report << (rank ? "," : "") << returned[rank];
        report << "]}";
      }
      const double recall = double(hits) / double(nq * k);
      report << "],\"recall_at_10\":" << recall << ",\"query_seconds\":" << query_seconds << '}';
      report.flush();
      std::cout << name << " ef=" << ef << " recall=" << recall << " query_seconds=" << query_seconds << std::endl;
    }
  }
  report << "],\"complete\":true}\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << "FAIL: " << e.what() << '\n';
  return 1;
}
