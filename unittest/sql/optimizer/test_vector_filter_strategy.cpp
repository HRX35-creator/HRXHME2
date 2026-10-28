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

#include <gtest/gtest.h>
#include "query/vector/ob_vector_index_util.h"
#include "sql/das/iter/ob_das_hnsw_scan_iter.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan_ctx.h"

namespace oceanbase
{
namespace share
{

static ObVecIdxExtraInfo make_info(int64_t rows, double selectivity, bool has_range)
{
  ObVecIdxExtraInfo info;
  info.row_count_ = rows;
  info.selectivity_ = selectivity;
  info.can_extract_range_ = has_range;
  return info;
}

TEST(VectorFilterStrategy, residual_scan_starts_with_ann_even_with_default_selectivity)
{
  // 0.05 is the estimate for the issue's BETWEEN without column statistics.
  // The strategy must not depend on whether that estimate is accurate.
  for (const auto rows : {100, 20000, 30000}) {
    for (const auto selectivity : {0.0, 0.0001, 0.05, 0.35, 1.0}) {
      auto info = make_info(rows, selectivity, false);
      ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(info, true));
      EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER, info.adaptive_try_path_);
    }
  }
}

TEST(VectorFilterStrategy, larger_selective_scans_keep_the_existing_adaptive_policy)
{
  // Outside the small-candidate shortcut, keep the established initial choice
  // and runtime retry thresholds rather than changing the whole cost policy.
  auto info = make_info(2000000, 0.05, false);
  ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(info, true));
  EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER, info.adaptive_try_path_);
}

TEST(VectorFilterStrategy, indexed_ranges_require_both_small_cardinality_and_selectivity)
{
  for (const auto rows : {100, 20000, 30000}) {
    for (const bool primary : {false, true}) {
      const double threshold = primary ? ObVecIdxExtraInfo::DEFAULT_PRE_RATE_FILTER_WITH_ROWKEY
                                       : ObVecIdxExtraInfo::DEFAULT_PRE_RATE_FILTER_WITH_IDX;
      auto selective = make_info(rows, threshold, true);
      ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(selective, primary));
      EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER, selective.adaptive_try_path_);
      auto unselective = make_info(rows, threshold + 0.001, true);
      ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(unselective, primary));
      EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER, unselective.adaptive_try_path_);
      auto all_match = make_info(rows, 1.0, true);
      ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(all_match, primary));
      EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER, all_match.adaptive_try_path_);
    }
  }
  auto primary_limit = make_info(4000000, 0.25, true);
  ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(primary_limit, true));
  EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER, primary_limit.adaptive_try_path_);
  auto secondary_limit = make_info(2000000, 0.15, true);
  ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(secondary_limit, false));
  EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER, secondary_limit.adaptive_try_path_);
}

TEST(VectorFilterStrategy, unknown_cardinality_preserves_index_merge_initial_path)
{
  auto info = make_info(0, 0.5, false);
  ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(info, false));
  EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER, info.adaptive_try_path_);
}

TEST(VectorFilterStrategy, explicit_paths_and_primary_infilter_are_preserved)
{
  for (const auto path : {ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER,
                          ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER,
                          ObVecIdxAdaTryPath::VEC_INDEX_IN_FILTER}) {
    auto info = make_info(20000, 1.0, false);
    info.adaptive_try_path_ = path;
    ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(info, true));
    EXPECT_EQ(path, info.adaptive_try_path_);
  }
  auto info = make_info(20000, 1.0, true);
  info.adaptive_try_path_ = ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER;
  info.with_extra_info_ = true;
  info.can_use_vec_pri_opt_ = true;
  ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(info, true));
  EXPECT_EQ(ObVecIdxAdaTryPath::VEC_INDEX_IN_FILTER, info.adaptive_try_path_);
}

TEST(VectorFilterStrategy, sparse_vector_policy_is_unchanged)
{
  for (const auto selectivity : {0.1, 0.1001}) {
    auto info = make_info(20000, selectivity, false);
    ASSERT_EQ(OB_SUCCESS, ObVectorIndexUtil::set_adaptive_try_path(info, true, true));
    EXPECT_EQ(selectivity <= ObVecIdxExtraInfo::DEFAULT_SINDI_SELECTIVITY_RATE
                ? ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER
                : ObVecIdxAdaTryPath::VEC_INDEX_POST_FILTER,
              info.adaptive_try_path_);
  }
}

} // namespace share

namespace sql
{

class HnswFilterFallbackTest : public ::testing::Test
{
protected:
  void observe(int64_t matches, int64_t candidates, int64_t rounds, bool primary = true)
  {
    iter_.adaptive_ctx_.row_count_ = 100;
    iter_.adaptive_ctx_.iter_res_row_cnt_ = matches;
    iter_.adaptive_ctx_.iter_filter_row_cnt_ = candidates;
    iter_.adaptive_ctx_.iter_times_ = rounds;
    iter_.adaptive_ctx_.is_primary_index_ = primary;
  }

  int retry(bool search_end) { return iter_.check_iter_filter_need_retry(search_end); }

  static void expect_remote_switch(share::ObVecIdxAdaTryPath initial, int64_t candidates)
  {
    common::ObArenaAllocator allocator;
    ObExecContext exec_ctx(allocator);
    ASSERT_EQ(OB_SUCCESS, exec_ctx.create_physical_plan_ctx());
    ASSERT_EQ(nullptr, exec_ctx.get_physical_plan_ctx()->get_phy_plan());
    ObDASHNSWScanIter iter;
    iter.exec_ctx_ = &exec_ctx;
    iter.vec_idx_try_path_ = initial;
    iter.adaptive_ctx_.iter_filter_row_cnt_ = candidates;
    iter.adaptive_ctx_.iter_res_row_cnt_ = candidates > 0 ? 1 : 0;
    iter.adaptive_ctx_.selectivity_ = 0.05;
    iter.can_retry_ = true;
    iter.go_brute_force_ = true;
    iter.query_cond_.ef_search_ = 64;
    iter.query_cond_.is_post_with_filter_ = true;

    ASSERT_EQ(OB_SUCCESS, iter.reset_filter_path());
    EXPECT_EQ(initial == share::ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER
                  ? share::ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER
                  : share::ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER,
              iter.vec_idx_try_path_);
    EXPECT_FALSE(iter.can_retry_);
    EXPECT_FALSE(iter.go_brute_force_);
    EXPECT_FALSE(iter.query_cond_.is_post_with_filter_);
    EXPECT_EQ(0, iter.query_cond_.ef_search_);
    if (initial == share::ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER) {
      EXPECT_DOUBLE_EQ(candidates > 0 ? 0.01 : 0.05, iter.adaptive_ctx_.selectivity_);
    }
  }

  ObDASHNSWScanIter iter_;
};

TEST_F(HnswFilterFallbackTest, exhausted_selective_search_can_retry_before_three_rounds)
{
  observe(1, 100, 1);
  EXPECT_EQ(OB_SUCCESS, retry(false));
  EXPECT_EQ(OB_VECTOR_INDEX_ADAPTIVE_NEED_RETRY, retry(true));
  observe(1, 100, 3);
  EXPECT_EQ(OB_VECTOR_INDEX_ADAPTIVE_NEED_RETRY, retry(false));
}

TEST_F(HnswFilterFallbackTest, exhausted_broad_or_unobserved_search_does_not_retry)
{
  observe(70, 100, 1);
  EXPECT_EQ(OB_SUCCESS, retry(true));
  observe(0, 0, 1);
  EXPECT_EQ(OB_SUCCESS, retry(true));
}

TEST_F(HnswFilterFallbackTest, index_selectivity_thresholds_still_apply_at_search_end)
{
  observe(20, 100, 1, false);
  EXPECT_EQ(OB_SUCCESS, retry(true));
  observe(20, 100, 1, true);
  EXPECT_EQ(OB_VECTOR_INDEX_ADAPTIVE_NEED_RETRY, retry(true));
}

TEST_F(HnswFilterFallbackTest, remote_retry_switches_paths_without_plan_cache_feedback)
{
  expect_remote_switch(share::ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER, 100);
  expect_remote_switch(share::ObVecIdxAdaTryPath::VEC_INDEX_PRE_FILTER, 100);
  expect_remote_switch(share::ObVecIdxAdaTryPath::VEC_INDEX_ITERATIVE_FILTER, 0);
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
