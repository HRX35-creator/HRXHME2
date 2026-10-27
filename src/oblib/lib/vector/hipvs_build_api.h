/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SeekdbHipvsBuildRequestV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t rows;
  uint32_t dimension;
  uint32_t max_degree;
  uint32_t ef_construction;
  uint32_t cpu_threads;
  const float *vectors;
};

struct SeekdbHipvsBuildResultV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint8_t *hnsw_data;
  uint64_t hnsw_bytes;
  double cagra_build_ms;  /* wall time including host transfer and stream synchronization */
  double hierarchy_serialize_ms;
  int32_t device_id;
  uint32_t nn_descent_iterations;
  char device_name[128];
  char gcn_arch[64];
  char error[512];
};

int seekdb_hipvs_build_hnsw_v1(const struct SeekdbHipvsBuildRequestV1 *request,
                              struct SeekdbHipvsBuildResultV1 *result);
void seekdb_hipvs_free_v1(void *buffer);

#ifdef __cplusplus
}
#endif
