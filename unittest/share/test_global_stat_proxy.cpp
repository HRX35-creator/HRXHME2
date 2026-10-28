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

#include <chrono>
#include <string>
#include <thread>
#include <gtest/gtest.h>
#include "common/mysqlclient/ob_isql_result_handler.h"
#include "lib/worker.h"
#include "share/ob_global_stat_proxy.h"
#include "share/ob_share_util.h"

namespace oceanbase
{
namespace share
{
using namespace common;
using namespace common::sqlclient;

// A fresh __all_global_stat has no rows. Exercise the real core-table proxy
// while replacing only SQL execution, without booting a server or doing I/O.
class EmptyCoreResult final : public ObMySQLResult, public ObISQLResultHandler
{
public:
  ObMySQLResult *mysql_result() override { return this; }
  int64_t get_column_count() const override { return 3; }
  int close() override { return OB_SUCCESS; }
  int next() override { return OB_ITER_END; }

#define UNUSED_GETTER(name, type) \
  int name(int64_t, type &) const override { return OB_NOT_SUPPORTED; } \
  int name(const char *, type &) const override { return OB_NOT_SUPPORTED; }
  UNUSED_GETTER(get_int, int64_t)
  UNUSED_GETTER(get_uint, uint64_t)
  UNUSED_GETTER(get_datetime, int64_t)
  UNUSED_GETTER(get_date, int32_t)
  UNUSED_GETTER(get_time, int64_t)
  UNUSED_GETTER(get_year, uint8_t)
  UNUSED_GETTER(get_bool, bool)
  UNUSED_GETTER(get_varchar, ObString)
  UNUSED_GETTER(get_float, float)
  UNUSED_GETTER(get_double, double)
  UNUSED_GETTER(get_type, ObObjMeta)
#undef UNUSED_GETTER
  int get_timestamp(int64_t, const ObTimeZoneInfo *, int64_t &) const override
  { return OB_NOT_SUPPORTED; }
  int get_timestamp(const char *, const ObTimeZoneInfo *, int64_t &) const override
  { return OB_NOT_SUPPORTED; }
  int get_obj(int64_t, ObObj &, const ObTimeZoneInfo *, ObIAllocator *) const override
  { return OB_NOT_SUPPORTED; }
  int get_obj(const char *, ObObj &) const override { return OB_NOT_SUPPORTED; }
private:
  int inner_get_number(int64_t, number::ObNumber &, IAllocator &) const override
  { return OB_NOT_SUPPORTED; }
  int inner_get_number(const char *, number::ObNumber &, IAllocator &) const override
  { return OB_NOT_SUPPORTED; }
};

class DelayedCoreClient final : public ObISQLClient
{
public:
  int escape(const char *, int64_t, char *, int64_t, int64_t &) override
  { return OB_NOT_SUPPORTED; }
  ObISQLConnection *get_connection() override { return nullptr; }

  int read(ReadResult &res, const char *sql, int32_t) override
  {
    ++reads_;
    read_deadline_ = ObTimeoutCtx::get_ctx().get_abs_timeout();
    read_sql_ = sql;
    std::this_thread::sleep_for(std::chrono::microseconds(read_delay_us_));
    if (ObTimeoutCtx::get_ctx().is_timeouted()) {
      return OB_TIMEOUT;
    }
    EmptyCoreResult *result = nullptr;
    return res.create_handler(result);
  }

  int write(const char *sql, int32_t, int64_t &affected_rows) override
  {
    ++writes_;
    write_deadline_ = ObTimeoutCtx::get_ctx().get_abs_timeout();
    write_sql_ = sql;
    std::this_thread::sleep_for(std::chrono::microseconds(write_delay_us_));
    affected_rows = 9;
    return ObTimeoutCtx::get_ctx().is_timeouted() ? OB_TIMEOUT : write_ret_;
  }

  int64_t read_delay_us_ = 0;
  int64_t write_delay_us_ = 0;
  int64_t read_deadline_ = -1;
  int64_t write_deadline_ = -1;
  int reads_ = 0;
  int writes_ = 0;
  int write_ret_ = OB_SUCCESS;
  std::string read_sql_;
  std::string write_sql_;
};

class TestGlobalStatProxy : public ::testing::Test
{
public:
  static constexpr int64_t SCHEMA_VERSION = 1;
  void SetUp() override
  {
    worker_timeout_ = THIS_WORKER.get_timeout_ts();
    THIS_WORKER.set_timeout_ts(INT64_MAX);
  }
  void TearDown() override { THIS_WORKER.set_timeout_ts(worker_timeout_); }

  int initialize()
  {
    ObGlobalStatProxy proxy(client_);
    return proxy.set_init_value(SCHEMA_VERSION, SCHEMA_VERSION,
        OB_INVALID_VERSION, SCN::min_scn(), 0, 0);
  }

  DelayedCoreClient client_;
  int64_t worker_timeout_ = INT64_MAX;
};

TEST_F(TestGlobalStatProxy, slow_bootstrap_read_leaves_time_for_initial_write)
{
  // As in #1544, the locking read fits in rpc_timeout, but the read and write
  // together exceed it. Bootstrap should use the internal SQL budget instead.
  client_.read_delay_us_ = config::rpc_timeout() * 9 / 10;
  client_.write_delay_us_ = config::rpc_timeout() / 5;
  ASSERT_GT(config::internal_sql_execute_timeout(),
      client_.read_delay_us_ + client_.write_delay_us_);
  EXPECT_EQ(OB_SUCCESS, initialize());
  EXPECT_EQ(1, client_.reads_);
  EXPECT_EQ(1, client_.writes_);
  EXPECT_EQ(client_.read_deadline_, client_.write_deadline_);
  EXPECT_NE(std::string::npos, client_.read_sql_.find("FOR UPDATE"));
  EXPECT_NE(std::string::npos, client_.write_sql_.find("'core_schema_version'"));
  EXPECT_NE(std::string::npos, client_.write_sql_.find("'snapshot_gc_scn'"));
  EXPECT_FALSE(ObTimeoutCtx::get_ctx().is_timeout_set());
}

TEST_F(TestGlobalStatProxy, preserves_caller_deadline)
{
  ObTimeoutCtx caller;
  ASSERT_EQ(OB_SUCCESS, caller.set_timeout(5 * 1000 * 1000));
  const int64_t deadline = caller.get_abs_timeout();
  EXPECT_EQ(OB_SUCCESS, initialize());
  EXPECT_EQ(deadline, client_.read_deadline_);
  EXPECT_EQ(deadline, client_.write_deadline_);
  EXPECT_EQ(deadline, ObTimeoutCtx::get_ctx().get_abs_timeout());
}

TEST_F(TestGlobalStatProxy, initialization_uses_internal_sql_budget)
{
  const int64_t before = ObTimeUtility::current_time();
  EXPECT_EQ(OB_SUCCESS, initialize());
  const int64_t after = ObTimeUtility::current_time();
  EXPECT_GE(client_.read_deadline_, before + config::internal_sql_execute_timeout());
  EXPECT_LE(client_.read_deadline_, after + config::internal_sql_execute_timeout());
  EXPECT_EQ(client_.read_deadline_, client_.write_deadline_);
  EXPECT_FALSE(ObTimeoutCtx::get_ctx().is_timeout_set());
}

TEST_F(TestGlobalStatProxy, preserves_worker_deadline)
{
  const int64_t deadline = ObTimeUtility::current_time() + 5 * 1000 * 1000;
  THIS_WORKER.set_timeout_ts(deadline);
  EXPECT_EQ(OB_SUCCESS, initialize());
  EXPECT_EQ(deadline, client_.read_deadline_);
  EXPECT_EQ(deadline, client_.write_deadline_);
  EXPECT_EQ(deadline, THIS_WORKER.get_timeout_ts());
  EXPECT_FALSE(ObTimeoutCtx::get_ctx().is_timeout_set());
}

TEST_F(TestGlobalStatProxy, expired_deadline_prevents_sql)
{
  ObTimeoutCtx caller;
  ASSERT_EQ(OB_SUCCESS, caller.set_abs_timeout(ObTimeUtility::current_time() - 1));
  EXPECT_EQ(OB_TIMEOUT, initialize());
  EXPECT_EQ(0, client_.reads_);
  EXPECT_EQ(0, client_.writes_);
}

TEST_F(TestGlobalStatProxy, caller_deadline_still_expires_during_sql)
{
  ObTimeoutCtx caller;
  ASSERT_EQ(OB_SUCCESS, caller.set_timeout(20 * 1000));
  client_.read_delay_us_ = 50 * 1000;
  EXPECT_EQ(OB_TIMEOUT, initialize());
  EXPECT_EQ(1, client_.reads_);
  EXPECT_EQ(0, client_.writes_);
}

TEST_F(TestGlobalStatProxy, write_failure_is_propagated_and_context_is_restored)
{
  client_.write_ret_ = OB_ERR_UNEXPECTED;
  EXPECT_EQ(OB_ERR_UNEXPECTED, initialize());
  EXPECT_EQ(1, client_.writes_);
  EXPECT_FALSE(ObTimeoutCtx::get_ctx().is_timeout_set());
}

TEST_F(TestGlobalStatProxy, ordinary_metadata_reads_keep_rpc_timeout)
{
  ObGlobalStatProxy proxy(client_);
  int64_t version = OB_INVALID_VERSION;
  const int64_t before = ObTimeUtility::current_time();
  EXPECT_EQ(OB_EMPTY_RESULT, proxy.get_core_schema_version(version));
  const int64_t after = ObTimeUtility::current_time();
  EXPECT_GE(client_.read_deadline_, before + config::rpc_timeout());
  EXPECT_LE(client_.read_deadline_, after + config::rpc_timeout());
  EXPECT_FALSE(ObTimeoutCtx::get_ctx().is_timeout_set());
}

TEST_F(TestGlobalStatProxy, invalid_initial_values_do_not_execute_sql)
{
  ObGlobalStatProxy proxy(client_);
  EXPECT_EQ(OB_INVALID_ARGUMENT,
      proxy.set_init_value(0, SCHEMA_VERSION, OB_INVALID_VERSION, SCN::min_scn(), 0, 0));
  EXPECT_EQ(0, client_.reads_);
  EXPECT_EQ(0, client_.writes_);
  EXPECT_FALSE(ObTimeoutCtx::get_ctx().is_timeout_set());
}

} // namespace share
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
