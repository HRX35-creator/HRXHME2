/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <stddef.h>
#include <stdint.h>

// Optional, separately built ROCm plugin. ABI v1 only supports dense float32 L2.
// Success returns the unmodified CAGRA HNSW export; caller must validate/convert
// it before VSAG loading. Failure returns no allocation. No CPU build fallback.
// Caller supplies a dedicated 32 MiB-stack thread and keeps inputs alive until
// return. build returns only after GPU synchronization and export completion.
// The allocation belongs to the plugin and must be released with its free API.
typedef int (*HipvsBuildV1)(const float *, int64_t, int32_t, int32_t,
                          unsigned char **, size_t *, char *, size_t);
typedef void (*HipvsFreeV1)(unsigned char *);

#ifdef __cplusplus
extern "C" {
#endif
int seekdb_hipvs_build_v1(const float *vectors, int64_t rows, int32_t dim, int32_t m,
                        unsigned char **bytes, size_t *length, char *error, size_t error_capacity);
void seekdb_hipvs_free_v1(unsigned char *bytes);
#ifdef __cplusplus
}
#endif
