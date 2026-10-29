/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "lib/vector/hipvs_build_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Uses public hipVS C APIs only. The temporary export is private (mkstemp),
// always removed, and its I/O is part of the measured build operation.
int seekdb_hipvs_build_v1(const float *vectors, int64_t rows, int32_t dim, int32_t m,
                        unsigned char **bytes, size_t *length, char *error, size_t error_capacity)
{
  int result = -1;
  cuvsResources_t resources = 0;
  cuvsCagraIndexParams_t params = NULL;
  cuvsCagraIndex_t index = NULL;
  FILE *file = NULL;
  unsigned char *buffer = NULL;
  char path[] = "/tmp/seekdb-hipvs-XXXXXX";
  int fd = -1;
  int created = 0;
  const char *failure = "hipVS build failed";
  if (bytes == NULL || length == NULL || error == NULL || error_capacity == 0) return -1;
  *bytes = NULL;
  *length = 0;
  error[0] = 0;
  if (vectors == NULL || dim <= 0 || m < 2 || m > 128 || rows <= 2 * m || rows >= UINT32_MAX ||
      (uint64_t)rows > (SIZE_MAX - 96) / (16 + 8 * (uint64_t)m + 4 * (uint64_t)dim)) {
    failure = "unsupported dense L2 build dimensions";
    goto cleanup;
  }
  for (uint64_t i = 0; i < (uint64_t)rows * dim; ++i) {
    if (!isfinite(vectors[i])) { failure = "non-finite input vector"; goto cleanup; }
  }
#define HIPVS_CALL(call) do { if ((call) != CUVS_SUCCESS) { failure = cuvsGetLastErrorText(); goto cleanup; } } while (0)
  HIPVS_CALL(cuvsResourcesCreate(&resources));
  HIPVS_CALL(cuvsCagraIndexParamsCreate(&params));
  HIPVS_CALL(cuvsCagraIndexCreate(&index));
  params->metric = L2Expanded;
  params->graph_degree = 2 * m;
  params->intermediate_graph_degree = rows - 1 < 4 * m ? rows - 1 : 4 * m;
  params->build_algo = NN_DESCENT;
  params->nn_descent_niter = 20;
  int64_t shape[2] = { rows, dim };
  DLManagedTensor dataset = {0};
  dataset.dl_tensor.data = (void *)vectors;
  dataset.dl_tensor.device.device_type = kDLCPU;
  dataset.dl_tensor.device.device_id = 0;
  dataset.dl_tensor.ndim = 2;
  dataset.dl_tensor.dtype.code = kDLFloat;
  dataset.dl_tensor.dtype.bits = 32;
  dataset.dl_tensor.dtype.lanes = 1;
  dataset.dl_tensor.shape = shape;
  HIPVS_CALL(cuvsCagraBuild(resources, params, &dataset, index));
  HIPVS_CALL(cuvsStreamSync(resources));
  fd = mkstemp(path);
  if (fd < 0) { failure = "cannot create private graph export"; goto cleanup; }
  created = 1;
  close(fd);
  fd = -1;
  HIPVS_CALL(cuvsCagraSerializeToHnswlib(resources, path, index));
  HIPVS_CALL(cuvsStreamSync(resources));
  const size_t expected = 96 + (size_t)rows * (16 + 8 * (size_t)m + 4 * (size_t)dim);
  file = fopen(path, "rb");
  if (file == NULL) { failure = "cannot read graph export"; goto cleanup; }
  buffer = (unsigned char *)malloc(expected);
  if (buffer == NULL) { failure = "cannot allocate graph export buffer"; goto cleanup; }
  if (fread(buffer, 1, expected, file) != expected || fgetc(file) != EOF || ferror(file)) {
    failure = "unexpected graph export length";
    goto cleanup;
  }
  *bytes = buffer;
  *length = expected;
  buffer = NULL;
  result = 0;
cleanup:
  // Capture the thread-local hipVS error before cleanup APIs can overwrite it.
  if (result != 0) snprintf(error, error_capacity, "%s", failure ? failure : "unknown hipVS error");
  if (file) fclose(file);
  if (fd >= 0) close(fd);
  if (created) unlink(path);
  free(buffer);
  if (index) cuvsCagraIndexDestroy(index);
  if (params) cuvsCagraIndexParamsDestroy(params);
  if (resources) cuvsResourcesDestroy(resources);
  return result;
#undef HIPVS_CALL
}

void seekdb_hipvs_free_v1(unsigned char *bytes) { free(bytes); }
