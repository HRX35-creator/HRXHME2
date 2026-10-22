/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "common/datum/datum_parameter.h"
#include "lib/container/ob_array_wrap.h"

namespace oceanbase
{
namespace common
{

// SQL batch values may have different types/accuracy even when element_ is
// available. Keep a complete native parameter for every value, not just a Datum
// and one shared type. Both descriptors and payloads belong to the caller arena.
struct DatumParameterArray
{
  DatumParameterArray() : data_(nullptr), count_(0), element_() {}

  static DatumParameterArray *alloc(ObIAllocator &allocator, int64_t count)
  {
    DatumParameterArray *array = nullptr;
    if (count >= 0 && count <= (INT64_MAX - sizeof(DatumParameterArray)) / sizeof(DatumParameter)) {
      const int64_t bytes = sizeof(DatumParameterArray) + sizeof(DatumParameter) * count;
      void *memory = allocator.alloc(bytes);
      if (memory != nullptr) {
        array = new (memory) DatumParameterArray();
        array->data_ = new (static_cast<char *>(memory) + sizeof(DatumParameterArray)) DatumParameter[count];
        array->count_ = count;
      }
    }
    return array;
  }

  typedef ObArrayWrap<DatumParameter> Values;
  TO_STRING_KV("data", Values(data_, count_), K_(count), K_(element));
  DatumParameter *data_;
  int64_t count_;
  ObDataType element_;
};

} // namespace common
} // namespace oceanbase
