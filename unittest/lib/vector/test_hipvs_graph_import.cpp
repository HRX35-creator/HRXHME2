/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "lib/vector/hipvs_graph_import.h"
#include <vsag/vsag.h>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>

using oceanbase::common::hipvs::GraphImport;
using Bytes = std::vector<unsigned char>;
constexpr size_t N = 65, DIM = 7, M = 16, DEGREE = 2 * M;
constexpr size_t VECTOR_OFFSET = 4 + DEGREE * 4;
constexpr size_t LABEL_OFFSET = VECTOR_OFFSET + DIM * 4;
constexpr size_t ROW_BYTES = LABEL_OFFSET + 8;
size_t checks = 0;
void check(bool condition, const char *message)
{
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
template<class T> void append(Bytes &b, T value)
{
  auto p = reinterpret_cast<const unsigned char *>(&value);
  b.insert(b.end(), p, p + sizeof(value));
}
template<class T> void set(Bytes &b, size_t offset, T value)
{
  std::memcpy(b.data() + offset, &value, sizeof(value));
}
template<class T> T take(tl::expected<T, vsag::Error> result)
{
  if (!result) throw std::runtime_error(result.error().message);
  return result.value();
}
void take(tl::expected<void, vsag::Error> result)
{
  if (!result) throw std::runtime_error(result.error().message);
}

// Independent fixture for the documented experimental hipVS export. This is a
// synthetic connected ring, NOT evidence that hipVS ran or of GPU performance.
Bytes fixture(const std::vector<float> &data)
{
  Bytes b;
  append<uint64_t>(b, 0); append<uint64_t>(b, N); append<uint64_t>(b, N);
  append<uint64_t>(b, ROW_BYTES); append<uint64_t>(b, LABEL_OFFSET);
  append<uint64_t>(b, VECTOR_OFFSET); append<int32_t>(b, 1); append<uint32_t>(b, N / 2);
  append<uint64_t>(b, M); append<uint64_t>(b, DEGREE); append<uint64_t>(b, M);
  append<double>(b, 0.42424242); append<uint64_t>(b, 500);
  for (size_t row = 0; row < N; ++row) {
    append<uint32_t>(b, DEGREE);
    for (size_t j = 1; j <= M; ++j) {
      append<uint32_t>(b, (row + j) % N);
      append<uint32_t>(b, (row + N - j) % N);
    }
    for (size_t d = 0; d < DIM; ++d) append<float>(b, data[row * DIM + d]);
    append<uint64_t>(b, row);
  }
  for (size_t row = 0; row < N; ++row) append<uint32_t>(b, 0);
  return b;
}

int main() try {
  std::cout << "vsag_version=" << vsag::version() << std::endl;
  std::vector<float> data(N * DIM);
  std::vector<int64_t> ids(N);
  for (size_t row = 0; row < N; ++row) {
    ids[row] = (row % 2 ? -1 : 1) * ((1LL << 40) + int64_t(row) * 101);
    for (size_t d = 0; d < DIM; ++d) data[row * DIM + d] = std::sin(float(row * DIM + d) * 0.3f);
  }
  std::mt19937 gen(183);
  std::shuffle(ids.begin(), ids.end(), gen);
  const Bytes original = fixture(data);
  auto reject = [&](Bytes b, const std::vector<float> &values, const std::vector<int64_t> &labels,
                    uint32_t dim = DIM, uint32_t m = M, uint64_t ef = 200) {
    const auto before = b;
    check(GraphImport::convert(b, values, labels, dim, m, ef) != nullptr, "invalid input accepted");
    check(b == before, "failure modified input");
  };
  // Every truncated length, including within the final row/layer, must fail.
  for (size_t length = 0; length < original.size(); ++length) {
    reject(Bytes(original.begin(), original.begin() + length), data, ids);
  }
  { auto b = original; b.push_back(0); reject(b, data, ids); }
  for (size_t offset : {0, 8, 16, 24, 32, 40, 48, 52, 56, 64, 72, 80, 88}) {
    auto b = original; b[offset] ^= 0x80; reject(b, data, ids);
  }
  for (size_t row : {size_t(0), N - 1}) {
    const size_t start = 96 + row * ROW_BYTES;
    { auto b = original; set<uint32_t>(b, start, DEGREE - 1); reject(b, data, ids); }
    { auto b = original; set<uint32_t>(b, start + 4, N); reject(b, data, ids); }
    { auto b = original; set<uint32_t>(b, start + 4, row); reject(b, data, ids); }
    { auto b = original; std::memcpy(b.data() + start + 8, b.data() + start + 4, 4); reject(b, data, ids); }
    { auto b = original; set<uint64_t>(b, start + LABEL_OFFSET, N); reject(b, data, ids); }
    { auto b = original; set<uint32_t>(b, 96 + N * ROW_BYTES + row * 4, 4); reject(b, data, ids); }
  }
  { auto d = data; d[DIM] += 1; reject(original, d, ids); }
  { auto d = data; std::swap_ranges(d.begin(), d.begin() + DIM, d.begin() + DIM); reject(original, d, ids); }
  for (float value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    auto d = data; d.back() = value; reject(fixture(d), d, ids);
  }
  { auto i = ids; i.back() = i.front(); reject(original, data, i); }
  { auto i = ids; i.pop_back(); reject(original, data, i); }
  { auto d = data; d.pop_back(); reject(original, d, ids); }
  reject(original, data, ids, 0); reject(original, data, ids, DIM, 0);
  reject(original, data, ids, DIM, 129); reject(original, data, ids, DIM, M, M);
  reject(original, data, ids, std::numeric_limits<uint32_t>::max());
  reject(original, {}, {});

  auto converted = original;
  check(GraphImport::convert(converted, data, ids, DIM, M, 200) == nullptr, "valid conversion failed");
  reject(converted, data, ids); // Never reinterpret already converted bytes as an export.
  // Graph edges and float payloads must remain byte-for-byte unchanged.
  for (size_t row = 0; row < N; ++row) {
    check(std::memcmp(original.data() + 96 + row * ROW_BYTES,
                      converted.data() + 96 + row * ROW_BYTES, LABEL_OFFSET) == 0,
          "conversion changed graph or vectors");
  }
  auto make_index = [] {
    return take(vsag::Factory::CreateIndex("hnsw",
      R"({"dtype":"float32","metric_type":"l2","dim":7,"use_old_serial_format":true,"hnsw":{"max_degree":16,"ef_construction":200,"ef_search":64}})"));
  };
  vsag::Binary blob;
  blob.size = converted.size(); blob.data.reset(new int8_t[blob.size]);
  std::memcpy(blob.data.get(), converted.data(), converted.size());
  vsag::BinarySet binary;
  binary.Set(vsag::HNSW_DATA, blob);
  auto index = make_index();
  // No CPU Build or Add is called to construct this imported index.
  take(index->Deserialize(binary));
  check(index->GetNumElements() == N, "import cardinality");
  std::stringstream persisted;
  take(index->Serialize(persisted));
  auto reloaded = make_index(); take(reloaded->Deserialize(persisted));
  check(reloaded->GetNumElements() == N, "reload cardinality");
  constexpr auto query_params = R"({"hnsw":{"ef_search":100}})";
  for (size_t row = 0; row < N; ++row) {
    auto query = vsag::Dataset::Make()->NumElements(1)->Dim(DIM)->Float32Vectors(data.data() + row * DIM)->Owner(false);
    auto result = take(index->KnnSearch(query, 10, query_params));
    auto again = take(reloaded->KnnSearch(query, 10, query_params));
    std::vector<std::pair<double, int64_t>> exact;
    for (size_t candidate = 0; candidate < N; ++candidate) {
      double distance = 0;
      for (size_t d = 0; d < DIM; ++d) {
        const double delta = double(data[row * DIM + d]) - data[candidate * DIM + d];
        distance += delta * delta;
      }
      exact.emplace_back(distance, ids[candidate]);
    }
    std::sort(exact.begin(), exact.end());
    check(result->GetDim() == 10 && again->GetDim() == 10, "top-k cardinality");
    for (size_t k = 0; k < 10; ++k) {
      check(result->GetIds()[k] == exact[k].second, "exact top-k mismatch");
      check(again->GetIds()[k] == result->GetIds()[k], "reload ID mismatch");
      check(std::abs(result->GetDistances()[k] - exact[k].first) < 1e-4, "distance mismatch");
    }
    std::function<bool(int64_t)> exclude = [&](int64_t id) { return id == ids[row]; };
    auto filtered = take(reloaded->KnnSearch(query, 10, query_params, exclude));
    check(filtered->GetDim() == 10, "filtered cardinality");
    for (size_t k = 0; k < 10; ++k) check(filtered->GetIds()[k] == exact[k + 1].second, "filter mismatch");
  }
  // Incremental CPU writes remain valid after a GPU snapshot and after reload.
  int64_t next_id = (1LL << 55) + 7;
  std::vector<float> next(DIM, 7);
  auto addition = vsag::Dataset::Make()->NumElements(1)->Dim(DIM)->Ids(&next_id)->Float32Vectors(next.data())->Owner(false);
  check(take(reloaded->Add(addition)).empty(), "incremental add rejected");
  auto query = vsag::Dataset::Make()->NumElements(1)->Dim(DIM)->Float32Vectors(next.data())->Owner(false);
  check(take(reloaded->KnnSearch(query, 1, query_params))->GetIds()[0] == next_id, "incremental query mismatch");
  std::stringstream extended;
  take(reloaded->Serialize(extended));
  auto last = make_index(); take(last->Deserialize(extended));
  check(last->GetNumElements() == N + 1, "extended reload cardinality");
  check(take(last->KnnSearch(query, 1, query_params))->GetIds()[0] == next_id, "extended reload mismatch");
  std::cout << "PASS: " << checks << " checks; 65 exact top-10 and filtered queries; persistence and incremental add\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << "FAIL: " << e.what() << '\n'; return 1;
}
