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

#include "common/datum/datum_core.h"

namespace oceanbase
{
namespace common
{

// A typed Datum view for constants. Metadata is adjacent to the native value;
// payload ownership belongs to the containing plan/statement arena. Parameter
// accuracy, result flags and wire state belong in DatumParameter, not here.
struct DatumValue
{
  DatumValue() : datum_(), meta_() { datum_.set_null(); }
  TO_STRING_KV(K_(datum), K_(meta));
  ObDatum datum_;
  ObObjMeta meta_;
};
static_assert(sizeof(DatumValue) == 16, "Typed native constants stay compact");

} // namespace common
} // namespace oceanbase
