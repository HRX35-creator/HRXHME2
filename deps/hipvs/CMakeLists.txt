cmake_minimum_required(VERSION 3.24)
project(seekdb_hipvs_bridge LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The hipVS build bridge requires Linux with AMD ROCm")
endif()
find_package(hip CONFIG REQUIRED)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use the ROCm HIP compiler: -DCMAKE_CXX_COMPILER=/opt/rocm/bin/hipcc")
endif()
# The C API installation can be used without the C++ RAFT development package.
# Some prebuilt hipVS images export a cuvs config that requires that absent
# package even for the C component, so locate only the ABI we actually consume.
set(HIPVS_ROOT "" CACHE PATH "hipVS C API installation prefix")
set(HIPVS_GPU_ARCH "native" CACHE STRING "AMD architecture supported by the hipVS installation")
find_path(HIPVS_C_API_INCLUDE_DIR cuvs/core/c_api.h HINTS "${HIPVS_ROOT}"
  PATH_SUFFIXES include REQUIRED)
find_library(HIPVS_C_API_LIBRARY NAMES cuvs_c HINTS "${HIPVS_ROOT}"
  PATH_SUFFIXES lib lib64 REQUIRED)
add_library(seekdb_hipvs_bridge SHARED bridge.cpp)
target_compile_features(seekdb_hipvs_bridge PRIVATE cxx_std_17)
target_compile_definitions(seekdb_hipvs_bridge PRIVATE __HIP_PLATFORM_AMD__)
target_compile_options(seekdb_hipvs_bridge PRIVATE -Wall -Wextra -Werror -x hip
  "--offload-arch=${HIPVS_GPU_ARCH}")
target_include_directories(seekdb_hipvs_bridge PRIVATE ../../src/oblib
  "${HIPVS_C_API_INCLUDE_DIR}" "${HIPVS_C_API_INCLUDE_DIR}/cuvs")
target_link_libraries(seekdb_hipvs_bridge PRIVATE "${HIPVS_C_API_LIBRARY}" hip::host)
install(TARGETS seekdb_hipvs_bridge LIBRARY DESTINATION lib)
