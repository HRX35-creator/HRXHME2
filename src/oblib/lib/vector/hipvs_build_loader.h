/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "hipvs_build_bridge.h"
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#if defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
#endif

namespace oceanbase::common::hipvs {

class BuildExport {
public:
  BuildExport() = default;
  BuildExport(const BuildExport &) = delete;
  BuildExport &operator=(const BuildExport &) = delete;
  ~BuildExport() { if (bytes_ != nullptr && free_ != nullptr) free_(bytes_); }
  unsigned char *bytes_ = nullptr;
  size_t length_ = 0;
  HipvsFreeV1 free_ = nullptr;
};

// No SDK is linked into ordinary CPU builds. lib=hipvs opts into this plugin;
// an absent plugin, failed GPU call, or unsupported export must fail the build.
inline bool build_export(const float *vectors, int64_t rows, int32_t dim, int32_t m,
                         BuildExport &output, std::string &error)
{
  error.clear();
  if (output.bytes_ != nullptr) { error = "output must be empty"; return false; }
#if defined(__linux__)
  struct Plugin {
    void *handle = nullptr;
    HipvsBuildV1 build = nullptr;
    HipvsFreeV1 release = nullptr;
    Plugin()
    {
      handle = dlopen("libseekdb_hipvs_build.so", RTLD_NOW | RTLD_LOCAL);
      if (handle != nullptr) {
        build = reinterpret_cast<HipvsBuildV1>(dlsym(handle, "seekdb_hipvs_build_v1"));
        release = reinterpret_cast<HipvsFreeV1>(dlsym(handle, "seekdb_hipvs_free_v1"));
      }
    }
    // Keep the GPU runtime loaded until process exit (including its TLS).
  };
  static const Plugin plugin;
  if (plugin.build == nullptr || plugin.release == nullptr) {
    error = "hipVS build plugin unavailable or ABI v1 missing; install it and restart";
    return false;
  }
  struct Task {
    const float *vectors;
    int64_t rows;
    int32_t dim, m;
    HipvsBuildV1 build;
    BuildExport *output;
    int status = -1;
    char error[512] = {};
  } task{vectors, rows, dim, m, plugin.build, &output};
  output.free_ = plugin.release;
  auto run = [](void *argument) -> void * {
    auto &task = *static_cast<Task *>(argument);
    try {
      task.status = task.build(task.vectors, task.rows, task.dim, task.m,
                               &task.output->bytes_, &task.output->length_, task.error, sizeof(task.error));
    } catch (...) {
      std::snprintf(task.error, sizeof(task.error), "exception escaped hipVS build plugin");
    }
    return nullptr;
  };
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) { error = "cannot initialize GPU worker attributes"; return false; }
  int status = pthread_attr_setstacksize(&attributes, 32 * 1024 * 1024);
  pthread_t worker;
  if (status == 0) status = pthread_create(&worker, &attributes, run, &task);
  pthread_attr_destroy(&attributes);
  if (status != 0) { error = "cannot create GPU build worker"; return false; }
  // This freshly-created joinable worker is joined exactly once. A join failure
  // cannot safely return with a worker still referencing the caller's buffers.
  if (pthread_join(worker, nullptr) != 0) std::terminate();
  task.error[sizeof(task.error) - 1] = 0;
  if (task.status != 0 || output.bytes_ == nullptr || output.length_ == 0) {
    error = task.error[0] ? task.error : "hipVS returned no graph";
    return false;
  }
  return true;
#else
  error = "hipVS build requires Linux with AMD ROCm";
  return false;
#endif
}
} // namespace oceanbase::common::hipvs
