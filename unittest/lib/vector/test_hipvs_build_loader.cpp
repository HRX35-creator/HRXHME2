/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "lib/vector/hipvs_build_loader.h"
#include <iostream>
#include <stdexcept>
using namespace oceanbase::common::hipvs;
void require(bool value) { if (!value) throw std::runtime_error("loader assertion failed"); }
int main(int argc, char **) try
{
  float values[65 * 7] = {};
  std::string error;
  if (argc == 1) {
    BuildExport output;
    require(!build_export(values, 65, 7, 16, output, error));
    require(!error.empty() && output.bytes_ == nullptr);
    std::cout << "PASS: absent plugin fails without CPU fallback\n";
  } else {
    void *plugin = dlopen("libseekdb_hipvs_build.so", RTLD_NOW | RTLD_LOCAL);
    require(plugin != nullptr);
    auto freed = reinterpret_cast<size_t (*)()>(dlsym(plugin, "test_freed"));
    auto stack = reinterpret_cast<size_t (*)()>(dlsym(plugin, "test_stack_bytes"));
    require(freed != nullptr && stack != nullptr);
    for (int64_t rows : {-1, 0, -2}) {
      BuildExport output;
      require(!build_export(values, rows, 7, 16, output, error));
      require(!error.empty());
    }
    require(freed() == 1);
    {
      BuildExport output;
      require(build_export(values, 66, 7, 16, output, error));
      require(output.length_ == 4 && output.bytes_ != nullptr);
      require(!build_export(values, 65, 7, 16, output, error));
      require(stack() >= 32 * 1024 * 1024);
    }
    require(freed() == 2);
    dlclose(plugin);
    std::cout << "PASS: worker stack, GPU failure, empty export, exception, ownership and cleanup\n";
  }
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n'; return 1;
}
