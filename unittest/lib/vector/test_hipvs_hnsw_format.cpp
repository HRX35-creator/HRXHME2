/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "lib/vector/hipvs_hnsw_format.h"
#include "lib/vector/hipvs_hnsw_builder.h"
#include "vsag/vsag.h"
#include "vsag/factory.h"
#include "vsag/options.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <vector>

using oceanbase::common::obvsag::HipvsHnswSource;
using oceanbase::common::obvsag::prepare_hipvs_hnsw_blob;
namespace {
void check(bool condition, const char *message)
{
  if (!condition) { throw std::runtime_error(message); }
}
uint64_t u64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
void put32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t *p, uint64_t v) { std::memcpy(p, &v, 8); }

std::vector<uint8_t> read_file(const char *path)
{
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  check(stream.is_open(), "cannot open external GPU fixture");
  const auto length = stream.tellg();
  check(length > 0 && length < 100000000, "invalid external GPU fixture length");
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  check(stream.good(), "cannot read complete external GPU fixture");
  return bytes;
}

std::shared_ptr<vsag::Index> empty_index()
{
  auto result = vsag::Factory::CreateIndex("hnsw", R"({"dtype":"float32","metric_type":"l2","dim":16,
    "hnsw":{"max_degree":8,"ef_construction":80,"use_conjugate_graph":false}})");
  check(result.has_value(), "native VSAG index creation failed");
  return result.value();
}

vsag::DatasetPtr data(float *vectors, int64_t *ids, int64_t rows)
{
  return vsag::Dataset::Make()->Dim(16)->NumElements(rows)->Float32Vectors(vectors)->Ids(ids)->Owner(false);
}

void load(std::shared_ptr<vsag::Index> index, const std::vector<uint8_t> &blob)
{
  vsag::BinarySet binaries;
  auto bytes = std::shared_ptr<int8_t[]>(new int8_t[blob.size()]);
  std::memcpy(bytes.get(), blob.data(), blob.size());
  binaries.Set(vsag::HNSW_DATA, {bytes, blob.size()});
  auto result = index->Deserialize(binaries);
  if (!result.has_value()) { throw std::runtime_error(result.error().message); }
}

void query(std::shared_ptr<vsag::Index> index, const std::vector<float> &vectors, const std::vector<int64_t> &ids)
{
  for (int64_t i = 0; i < 200; ++i) {
    const size_t ordinal = (i * 17) % ids.size();
    auto q = vsag::Dataset::Make()->Dim(16)->NumElements(1)
      ->Float32Vectors(vectors.data() + ordinal * 16)->Owner(false);
    auto found = index->KnnSearch(q, 1, R"({"hnsw":{"ef_search":100}})");
    check(found.has_value(), "native VSAG query failed");
    check(found.value()->GetDim() == 1 && found.value()->GetIds()[0] == ids[ordinal], "native VSAG query returned the wrong source VID");
    check(found.value()->GetDistances()[0] < 1e-5f, "native VSAG changed source vectors");
  }
}

void independent_recall(std::shared_ptr<vsag::Index> baseline, std::shared_ptr<vsag::Index> imported,
                        const std::vector<float> &vectors, const std::vector<int64_t> &ids)
{
  std::mt19937 random(20261028);
  std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
  size_t cpu_hits = 0, imported_hits = 0;
  for (size_t q = 0; q < 200; ++q) {
    float values[16];
    for (float &v : values) { v = uniform(random); }
    std::vector<std::pair<double, size_t>> distances(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
      double distance = 0;
      for (size_t j = 0; j < 16; ++j) {
        const double delta = double(vectors[i * 16 + j]) - double(values[j]);
        distance += delta * delta;
      }
      distances[i] = {distance, i};
    }
    std::partial_sort(distances.begin(), distances.begin() + 10, distances.end());
    std::unordered_set<int64_t> truth_ordinals, truth_ids;
    for (size_t j = 0; j < 10; ++j) {
      truth_ordinals.insert(int64_t(distances[j].second));
      truth_ids.insert(ids[distances[j].second]);
    }
    auto input = vsag::Dataset::Make()->Dim(16)->NumElements(1)->Float32Vectors(values)->Owner(false);
    const auto hits = [&](std::shared_ptr<vsag::Index> index, const std::unordered_set<int64_t> &truth) {
      auto found = index->KnnSearch(input, 10, R"({"hnsw":{"ef_search":100}})");
      check(found.has_value() && found.value()->GetDim() == 10, "independent query lost neighbors");
      size_t count = 0;
      std::unordered_set<int64_t> seen;
      for (size_t j = 0; j < 10; ++j) {
        const int64_t id = found.value()->GetIds()[j];
        check(seen.insert(id).second, "independent query returned duplicate IDs");
        check(std::isfinite(found.value()->GetDistances()[j]) && found.value()->GetDistances()[j] >= 0,
              "independent query returned an invalid distance");
        count += truth.count(id);
      }
      return count;
    };
    cpu_hits += hits(baseline, truth_ordinals);
    imported_hits += hits(imported, truth_ids);
  }
  std::cout << "Independent Recall@10 (200 queries, float64 exact L2): CPU="
    << double(cpu_hits) / 2000 << ", imported=" << double(imported_hits) / 2000 << '\n';
  // A local small-fixture regression floor, not the competition acceptance threshold.
  check(cpu_hits >= 1900 && imported_hits >= 1900, "independent Recall@10 fell below the local 0.95 regression floor");
}
}  // namespace

int main(int argc, char **argv)
{
  try {
    check(argc == 1 || argc == 3, "usage: test_hipvs_hnsw_format [5000x16-float32-vectors hnsw-blob]");
    constexpr size_t N = 5000;
    vsag::Options::Instance().set_num_threads_building(2);
    std::mt19937 random(20261027);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    std::vector<float> vectors(N * 16), reversed(N * 16);
    std::vector<int64_t> ordinals(N), ids(N);
    for (float &v : vectors) { v = uniform(random); }
    if (argc == 3) {
      const auto bytes = read_file(argv[1]);
      check(bytes.size() == vectors.size() * sizeof(float), "external vector fixture has the wrong shape");
      std::memcpy(vectors.data(), bytes.data(), bytes.size());
    }
    for (size_t i = 0; i < N; ++i) {
      const size_t ordinal = N - i - 1;
      ordinals[i] = int64_t(ordinal);
      std::copy_n(vectors.data() + ordinal * 16, 16, reversed.data() + i * 16);
      ids[i] = i % 2 ? INT64_MIN + int64_t(i) : (int64_t(1) << 54) + int64_t(i);
    }
    auto original = empty_index();
    auto built = original->Build(data(reversed.data(), ordinals.data(), N));
    check(built.has_value() && built.value().empty(), "native VSAG baseline build failed");
    auto serialized = original->Serialize();
    check(serialized.has_value(), "native VSAG baseline serialization failed");
    auto binary = serialized.value().Get(vsag::HNSW_DATA);
    std::vector<uint8_t> raw(binary.size);
    std::memcpy(raw.data(), binary.data.get(), raw.size());
    if (argc == 3) { raw = read_file(argv[2]); }
    check(raw.size() >= 96, "external HNSW fixture is shorter than its header");
    const auto stride = u64(raw.data() + 24), label = u64(raw.data() + 32), vector_offset = u64(raw.data() + 40);
    if (argc == 1) {
      check(u64(raw.data() + 96 + label) != 0, "fixture did not exercise reordered internal nodes");
    }
    HipvsHnswSource source{vectors.data(), ids.data(), N, 16, 8, 80};
    std::string error;
    auto converted = raw;
    check(prepare_hipvs_hnsw_blob(converted.data(), converted.size(), source, error), error.c_str());
    auto imported = empty_index(); load(imported, converted);
    check(imported->GetNumElements() == N, "native VSAG lost imported rows");
    query(imported, vectors, ids);
    independent_recall(original, imported, vectors, ids);
    auto saved = imported->Serialize();
    check(saved.has_value(), "native imported index serialization failed");
    auto restored = empty_index();
    check(restored->Deserialize(saved.value()).has_value(), "native imported index restart readback failed");
    query(restored, vectors, ids);
    int64_t new_id = 4321;
    std::vector<float> new_vector(16, 4.0f);
    check(restored->Add(data(new_vector.data(), &new_id, 1)).has_value(), "imported HNSW is not mutable");
    check(restored->GetNumElements() == N + 1, "imported HNSW lost a later insert");
    auto removed = restored->Remove(new_id);
    check(removed.has_value() && removed.value(), "imported HNSW lost deletion semantics");
    size_t rejected = 0;
    const auto invalid = [&](std::vector<uint8_t> blob, size_t length, const HipvsHnswSource &expected) {
      const auto before = blob;
      check(!prepare_hipvs_hnsw_blob(blob.data(), length, expected, error), "corrupt HNSW was accepted");
      check(!error.empty() && blob == before, "failed validation partially changed the source blob");
      ++rejected;
    };
    invalid(raw, 95, source); invalid(raw, raw.size() - 1, source);
    auto bad = raw; bad.push_back(0); invalid(bad, bad.size(), source);
    bad = raw; put64(bad.data() + 16, N - 1); invalid(bad, bad.size(), source);
    bad = raw; put64(bad.data() + 56, 7); invalid(bad, bad.size(), source);
    bad = raw; put32(bad.data() + 52, N); invalid(bad, bad.size(), source);
    bad = raw; put64(bad.data() + 96 + label, N); invalid(bad, bad.size(), source);
    bad = raw; put64(bad.data() + 96 + stride + label, u64(raw.data() + 96 + label)); invalid(bad, bad.size(), source);
    bad = raw; bad[96 + vector_offset] ^= 1; invalid(bad, bad.size(), source);
    bad = raw; put32(bad.data() + 96, 17); invalid(bad, bad.size(), source);
    bad = raw; put32(bad.data() + 96, 1); put32(bad.data() + 100, N); invalid(bad, bad.size(), source);
    auto duplicate_ids = ids; duplicate_ids[1] = duplicate_ids[0];
    auto duplicate_source = source; duplicate_source.ids = duplicate_ids.data(); invalid(raw, raw.size(), duplicate_source);
    bad = raw; put64(bad.data() + 80, 1); invalid(bad, bad.size(), source);
#if defined(_WIN32)
    _putenv_s("SEEKDB_HIPVS_LIBRARY", "/__seekdb_hipvs_test_missing__/bridge.so");
#else
    setenv("SEEKDB_HIPVS_LIBRARY", "/__seekdb_hipvs_test_missing__/bridge.so", 1);
#endif
    vsag::BinarySet missing_output;
    oceanbase::common::obvsag::HipvsBuildReport missing_report;
    check(!oceanbase::common::obvsag::build_hipvs_hnsw(source, 2, missing_output, missing_report), "missing bridge was counted as a GPU build");
    check(!missing_report.error.empty() && missing_output.GetKeys().empty(), "missing bridge returned an index");
    std::cout << "PASS native VSAG " << vsag::version() << ": " << N
      << " rows, 400 exact self queries, signed/wide VIDs, persistence, mutation, "
      << rejected << " corrupt inputs rejected. "
      << (argc == 3 ? "External AMD bridge artifact; execution provenance recorded separately.\n"
                    : "Reordered CPU fixture; GPU execution not tested.\n");
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
  }
}
