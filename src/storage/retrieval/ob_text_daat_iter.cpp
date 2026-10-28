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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"
#include "ob_sparse_retrieval_util.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_project_relevance()
      && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    union_only_ = param.union_only_
        && !param.function_lookup_mode_
        && sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE == param.mode_flag_
        && !param.base_param_->need_project_relevance()
        && !param.base_param_->need_filter();
    if (union_only_ && param.dim_iters_->count() > 0) {
      const sql::ObDatumMeta &id_meta = param.base_param_->id_proj_expr_->datum_meta_;
      common::ObDatumBasicFuncs *basic_funcs = ObDatumFuncs::get_basic_func(id_meta.type_, id_meta.cs_type_);
      if (is_binary_domain_id(id_meta.type_, id_meta.cs_type_)) {
        union_id_kind_ = UNION_ID_BINARY;
      } else if (id_meta.type_ == ObUInt64Type) {
        union_id_kind_ = UNION_ID_UINT64;
      } else {
        union_id_kind_ = UNION_ID_GENERIC;
      }
      union_ids_.set_allocator(param.allocator_);
      if (OB_ISNULL(basic_funcs) || OB_ISNULL(union_cmp_func_ = basic_funcs->null_first_cmp_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(union_ids_.init(param.dim_iters_->count()))) {
      } else if (OB_FAIL(union_ids_.prepare_allocate(param.dim_iters_->count(), nullptr))) {
      }
    }
  }
  return ret;
}

inline int ObTextDaaTIter::compare_union_ids(const ObDatum &left, const ObDatum &right, int &cmp_ret) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(left.is_null() || right.is_null() || UNION_ID_GENERIC == union_id_kind_)) {
    ret = union_cmp_func_(left, right, cmp_ret, nullptr);
  } else if (UNION_ID_BINARY == union_id_kind_) {
    cmp_ret = compare_binary_domain_id(left, right);
  } else {
    cmp_ret = (left.get_uint64() > right.get_uint64()) - (left.get_uint64() < right.get_uint64());
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!union_only_) {
    ret = ObSRDaaTIterImpl::get_next_rows(capacity, count);
  } else if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (iter_param_->limit_param_->is_valid() && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else {
    count = 0;
    if (OB_FAIL(union_next_rows(MIN(capacity, iter_param_->max_batch_size_), count))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
      }
    }
    if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0) {
      int tmp_ret = project_results(count);
      ret = OB_SUCCESS == tmp_ret ? ret : tmp_ret;
    }
  }
  return ret;
}

// Without scores or a filter, a natural-language match returns the union of the posting lists.
// The lists are already ordered by document id, so the smallest current id is emitted once and
// every list positioned on it advances. This skips the merge heap and the relevance collector,
// whose per-posting bookkeeping dominates scalar aggregates such as COUNT(*).
int ObTextDaaTIter::union_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  const int64_t dim_cnt = dim_iters_->count();
  ObISRDaaTDimIter **dim_iters = dim_iters_->get_data();
  const ObDocIdExt **cur_ids = union_ids_.get_data();
  int64_t *pending = next_round_iter_idxes_.get_data();
  ObDocIdExt *buffered_ids = buffered_domain_ids_.get_data();
  const common::ObLimitParam &limit_param = *iter_param_->limit_param_;
  const bool has_limit = limit_param.is_valid();
  while (OB_SUCC(ret) && count < capacity) {
    // Advance the lists that produced the previous id. An exhausted list keeps a null id.
    for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
      const int64_t iter_idx = pending[i];
      ObTextRetrievalDaaTTokenIter *dim_iter = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters[iter_idx]);
      if (OB_FAIL(dim_iter->next_doc_id(cur_ids[iter_idx]))) {
        if (OB_LIKELY(OB_ITER_END == ret)) {
          ret = OB_SUCCESS;
          cur_ids[iter_idx] = nullptr;
        }
      }
    }
    next_round_cnt_ = 0;
    const ObDocIdExt *min_id = nullptr;
    const ObDatum *min_datum = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_cnt; ++i) {
      int cmp_ret = 0;
      if (nullptr == cur_ids[i]) {
      } else if (nullptr == min_id) {
        min_id = cur_ids[i];
        min_datum = &min_id->get_datum();
        pending[0] = i;
        next_round_cnt_ = 1;
      } else if (OB_FAIL(compare_union_ids(cur_ids[i]->get_datum(), *min_datum, cmp_ret))) {
      } else if (cmp_ret < 0) {
        min_id = cur_ids[i];
        min_datum = &min_id->get_datum();
        pending[0] = i;
        next_round_cnt_ = 1;
      } else if (0 == cmp_ret) {
        pending[next_round_cnt_++] = i;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (nullptr == min_id) {
      next_round_cnt_ = 0;
      ret = OB_ITER_END;
    } else if (FALSE_IT(++input_row_cnt_)) {
    } else if (has_limit && input_row_cnt_ <= limit_param.offset_) {
      // skipped by OFFSET
    } else {
      buffered_ids[count++] = *min_id;
      ++output_row_cnt_;
      if (has_limit && output_row_cnt_ >= limit_param.limit_) {
        ret = OB_ITER_END;
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  union_ids_.reset();
  union_only_ = false;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  for (int64_t i = 0; i < union_ids_.count(); ++i) {
    union_ids_[i] = nullptr;
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

bool ObTextBMWIter::prefer_exhaustive_search()
{
  int ret = OB_SUCCESS;
  int64_t total_postings = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    int64_t token_doc_cnt = 0;
    if (OB_FAIL(static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->get_token_doc_cnt(token_doc_cnt))) {
    } else {
      total_postings += token_doc_cnt;
    }
  }
  return OB_SUCC(ret) && total_postings <= EXHAUSTIVE_SEARCH_MAX_POSTINGS;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
