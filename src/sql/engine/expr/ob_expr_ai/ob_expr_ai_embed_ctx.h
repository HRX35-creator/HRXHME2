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

#include "sql/engine/expr/ob_expr_operator.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace sql
{

// Per-expression state that lets AI_EMBED fold consecutive rows into a single
// remote request even when the enclosing expression is evaluated row by row
// (for example `json_length(ai_embed(...))`). The operator keeps a bounded
// window of pending inputs; once the window is full it issues one request and
// serves the buffered rows from the produced vectors.
class ObExprAIEmbedCtx : public ObExprOperatorCtx
{
public:
  // A window large enough to amortize the round trip while keeping the request
  // body small when a scan projects many rows.
  static constexpr int64_t BATCH_CAPACITY = 16;

  ObExprAIEmbedCtx()
    : pending_inputs_(), pending_results_(), next_served_(0), has_pending_(false) {}

  void reset()
  {
    pending_inputs_.reuse();
    pending_results_.reuse();
    next_served_ = 0;
    has_pending_ = false;
  }

  bool has_pending() const { return has_pending_ && next_served_ < pending_results_.count(); }
  bool is_full() const { return pending_inputs_.count() >= BATCH_CAPACITY; }

  common::ObSEArray<common::ObString, BATCH_CAPACITY> pending_inputs_;
  common::ObSEArray<common::ObString, BATCH_CAPACITY> pending_results_;
  int64_t next_served_;
  bool has_pending_;
};

} // namespace sql
} // namespace oceanbase
