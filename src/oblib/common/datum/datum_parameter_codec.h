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

#include "common/datum/datum_parameter_array.h"
#include "common/datum/datum_param_store.h"

namespace oceanbase
{
namespace common
{

// Legacy value/parameter wire format implemented directly over native Datum.
// Parameter wire contains value + accuracy + result flags only. Runtime flags,
// source text positions and param_meta are intentionally absent from that format.
// SQL-array nesting is bounded at 64; PL pointer values remain unsupported by
// the legacy format. Extend transient size is not present in that wire format.
// Decoding owns every payload (including nested array elements) in allocator.
// Single-value decode failures preserve output and caller position; arena bytes
// allocated before an error remain owned by the caller until arena reset.
class DatumParameterCodec
{
public:
  // Store wire is the existing count + parameter sequence. A failed store
  // decode leaves the store empty and the caller position unchanged; payload
  // allocations remain in allocator, as for the single-value decoder.
  static int store_size(const DatumParamStore &store, int64_t &size);
  static int encode_store(const DatumParamStore &store, char *buf, int64_t length, int64_t &pos);
  static int decode_store(const char *buf, int64_t length, int64_t &pos,
                          ObIAllocator &allocator, DatumParamStore &store);

  static int value_size(const ObDatum &value, const ObObjMeta &meta, int64_t &size);
  static int encode_value(const ObDatum &value, const ObObjMeta &meta,
                          char *buf, int64_t length, int64_t &pos);
  static int decode_value(const char *buf, int64_t length, int64_t &pos,
                          ObIAllocator &allocator, ObDatum &value, ObObjMeta &meta);
  static int parameter_size(const DatumParameter &value, int64_t &size);
  static int encode_parameter(const DatumParameter &value, char *buf,
                              int64_t length, int64_t &pos);
  static int decode_parameter(const char *buf, int64_t length, int64_t &pos,
                              ObIAllocator &allocator, DatumParameter &value);
};

} // namespace common
} // namespace oceanbase
