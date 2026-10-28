/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Test-only ABI fault injection. Never install this as a GPU plugin.
#include "lib/vector/hipvs_build_bridge.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <vector>
static size_t freed = 0, stack_bytes = 0;
extern "C" size_t test_freed() { return freed; }
extern "C" size_t test_stack_bytes() { return stack_bytes; }
extern "C" int seekdb_hipvs_build_v1(const float *vectors, int64_t rows, int32_t dim, int32_t m,
    unsigned char **bytes, size_t *length, char *error, size_t capacity)
{
  pthread_attr_t attributes;
  pthread_getattr_np(pthread_self(), &attributes);
  pthread_attr_getstacksize(&attributes, &stack_bytes);
  pthread_attr_destroy(&attributes);
  if (rows == -2) throw std::runtime_error("test fault");
  if (rows == 0) return 0; // Invalid success without a graph.
  if (rows == 65 && dim == 7 && m == 16) {
    // Fixed-size SQL lifecycle fixture only; never a performance backend.
    std::vector<unsigned char> export_bytes;
    auto append = [&](auto value) {
      const auto *p = reinterpret_cast<const unsigned char *>(&value);
      export_bytes.insert(export_bytes.end(), p, p + sizeof(value));
    };
    append(uint64_t(0)); append(uint64_t(rows)); append(uint64_t(rows));
    append(uint64_t(168)); append(uint64_t(160)); append(uint64_t(132));
    append(int32_t(1)); append(uint32_t(rows / 2));
    append(uint64_t(m)); append(uint64_t(32)); append(uint64_t(m));
    append(double(0.42424242)); append(uint64_t(500));
    for (int64_t row = 0; row < rows; ++row) {
      append(uint32_t(32));
      for (int j = 1; j <= 16; ++j) {
        append(uint32_t((row + j) % rows)); append(uint32_t((row + rows - j) % rows));
      }
      for (int d = 0; d < dim; ++d) append(vectors[row * dim + d]);
      append(uint64_t(row));
    }
    for (int64_t row = 0; row < rows; ++row) append(uint32_t(0));
    *bytes = static_cast<unsigned char *>(std::malloc(export_bytes.size()));
    if (*bytes == nullptr) return -1;
    *length = export_bytes.size();
    std::memcpy(*bytes, export_bytes.data(), *length);
    std::fprintf(stderr, "TEST_ONLY_SYNTHETIC_GRAPH rows=65 dim=7\n");
    return 0;
  }
  *bytes = static_cast<unsigned char *>(std::calloc(4, 1));
  *length = 4;
  if (rows == -1) {
    std::snprintf(error, capacity, "injected GPU failure");
    return -1;
  }
  return 0;
}
extern "C" void seekdb_hipvs_free_v1(unsigned char *bytes) { ++freed; std::free(bytes); }
