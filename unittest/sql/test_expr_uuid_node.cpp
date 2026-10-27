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

#include "sql/engine/expr/ob_expr_uuid.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <cstring>
#include <gtest/gtest.h>

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{
constexpr int64_t NODE_LEN = ObUUIDNode::NODE_LENGTH;
const unsigned char HW_MAC[NODE_LEN] = {0x02, 0x42, 0xac, 0x11, 0x00, 0x02};
const unsigned char RANDOM_NODE[NODE_LEN] = {0x5b, 0x10, 0x20, 0x30, 0x40, 0x50};
int random_node_calls = 0;

int mac_success(unsigned char *node, const int64_t len)
{
  MEMCPY(node, HW_MAC, len);
  return OB_SUCCESS;
}

int mac_not_found(unsigned char *, const int64_t)
{
  return OB_ENTRY_NOT_EXIST;
}

int mac_permission_denied(unsigned char *node, const int64_t len)
{
  // partially written output must not leak into the published node
  MEMSET(node, 0xee, len);
  errno = EPERM;
  return OB_ERR_SYS;
}

int fake_random_node(unsigned char *node, const int64_t len)
{
  ++random_node_calls;
  MEMCPY(node, RANDOM_NODE, len);
  return OB_SUCCESS;
}

int failing_random_node(unsigned char *, const int64_t)
{
  ++random_node_calls;
  return OB_ERR_SYS;
}

int zero_entropy(unsigned char *buf, const int64_t len)
{
  MEMSET(buf, 0, len);
  return OB_SUCCESS;
}

int failing_entropy(unsigned char *, const int64_t)
{
  return OB_ERR_SYS;
}

int getrandom_calls = 0;

long getrandom_eperm(void *, size_t, unsigned int)
{
  ++getrandom_calls;
  errno = EPERM;
  return -1;
}

long getrandom_enosys(void *, size_t, unsigned int)
{
  ++getrandom_calls;
  errno = ENOSYS;
  return -1;
}

long getrandom_eio(void *, size_t, unsigned int)
{
  ++getrandom_calls;
  errno = EIO;
  return -1;
}

// EINTR first, then one byte per call
long getrandom_interrupted_partial(void *buf, size_t len, unsigned int)
{
  long ret = -1;
  if (0 == getrandom_calls++) {
    errno = EINTR;
  } else if (len > 0) {
    static_cast<unsigned char *>(buf)[0] = static_cast<unsigned char>(0xa0 + getrandom_calls);
    ret = 1;
  } else {
    ret = 0;
  }
  return ret;
}

// a regular file with known content stands in for /dev/urandom
class FallbackFile
{
public:
  explicit FallbackFile(const unsigned char *data, const int64_t len)
  {
    strcpy(path_, "/tmp/test_expr_uuid_node_XXXXXX");
    fd_ = mkstemp(path_);
    if (fd_ >= 0 && len > 0) {
      ok_ = (write(fd_, data, len) == len);
    } else {
      ok_ = (fd_ >= 0);
    }
  }
  ~FallbackFile()
  {
    if (fd_ >= 0) {
      close(fd_);
      unlink(path_);
    }
  }
  bool ok() const { return ok_; }
  const char *path() const { return path_; }

private:
  char path_[64];
  int fd_ = -1;
  bool ok_ = false;
};

const unsigned char FALLBACK_BYTES[NODE_LEN] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};

ObUUIDNode make_uninited_node()
{
  ObUUIDNode node;
  MEMSET(node.mac_addr_, 0, sizeof(node.mac_addr_));
  node.is_inited_ = false;
  return node;
}
}  // namespace

class TestExprUuidNode : public ::testing::Test
{
protected:
  void SetUp() override
  {
    random_node_calls = 0;
    getrandom_calls = 0;
  }
};

TEST_F(TestExprUuidNode, uses_mac_address_when_available)
{
  ObUUIDNode node = make_uninited_node();
  ASSERT_EQ(OB_SUCCESS, node.init(mac_success, fake_random_node));
  EXPECT_TRUE(node.is_inited_);
  EXPECT_EQ(0, MEMCMP(HW_MAC, node.mac_addr_, NODE_LEN));
  EXPECT_EQ(0, random_node_calls);
}

TEST_F(TestExprUuidNode, falls_back_when_no_usable_mac_address)
{
  ObUUIDNode node = make_uninited_node();
  ASSERT_EQ(OB_SUCCESS, node.init(mac_not_found, fake_random_node));
  EXPECT_TRUE(node.is_inited_);
  EXPECT_EQ(0, MEMCMP(RANDOM_NODE, node.mac_addr_, NODE_LEN));
  EXPECT_EQ(1, random_node_calls);
}

TEST_F(TestExprUuidNode, falls_back_on_interface_permission_error)
{
  ObUUIDNode node = make_uninited_node();
  ASSERT_EQ(OB_SUCCESS, node.init(mac_permission_denied, fake_random_node));
  EXPECT_TRUE(node.is_inited_);
  EXPECT_EQ(0, MEMCMP(RANDOM_NODE, node.mac_addr_, NODE_LEN));
  EXPECT_EQ(1, random_node_calls);
}

TEST_F(TestExprUuidNode, fails_without_publishing_when_entropy_fails)
{
  ObUUIDNode node = make_uninited_node();
  const unsigned char zero[NODE_LEN] = {0};
  EXPECT_NE(OB_SUCCESS, node.init(mac_permission_denied, failing_random_node));
  EXPECT_FALSE(node.is_inited_);
  EXPECT_EQ(0, MEMCMP(zero, node.mac_addr_, NODE_LEN));
}

TEST_F(TestExprUuidNode, failed_reinit_clears_inited_flag)
{
  ObUUIDNode node = make_uninited_node();
  ASSERT_EQ(OB_SUCCESS, node.init(mac_success, fake_random_node));
  EXPECT_NE(OB_SUCCESS, node.init(mac_not_found, failing_random_node));
  EXPECT_FALSE(node.is_inited_);
}

TEST_F(TestExprUuidNode, rejects_null_providers)
{
  ObUUIDNode node = make_uninited_node();
  EXPECT_EQ(OB_INVALID_ARGUMENT, node.init(nullptr, fake_random_node));
  EXPECT_EQ(OB_INVALID_ARGUMENT, node.init(mac_success, nullptr));
  EXPECT_FALSE(node.is_inited_);
}

TEST_F(TestExprUuidNode, random_node_sets_multicast_bit)
{
  unsigned char node[NODE_LEN] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::gen_random_node(zero_entropy, node, NODE_LEN));
  EXPECT_EQ(0x01, node[0]);
  for (int64_t i = 1; i < NODE_LEN; ++i) {
    EXPECT_EQ(0, node[i]);
  }
}

TEST_F(TestExprUuidNode, random_node_propagates_entropy_failure)
{
  unsigned char node[NODE_LEN] = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
  EXPECT_EQ(OB_ERR_SYS, ObUUIDNode::gen_random_node(failing_entropy, node, NODE_LEN));
  EXPECT_EQ(0x7f, node[0]);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObUUIDNode::gen_random_node(zero_entropy, node, NODE_LEN - 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObUUIDNode::gen_random_node(nullptr, node, NODE_LEN));
}

TEST_F(TestExprUuidNode, os_entropy_produces_distinct_values)
{
  unsigned char a[32] = {0};
  unsigned char b[32] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::get_os_entropy(a, sizeof(a)));
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::get_os_entropy(b, sizeof(b)));
  EXPECT_NE(0, MEMCMP(a, b, sizeof(a)));
}

TEST_F(TestExprUuidNode, process_random_node_is_stable_multicast)
{
  unsigned char a[NODE_LEN] = {0};
  unsigned char b[NODE_LEN] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::get_process_random_node(a, NODE_LEN));
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::get_process_random_node(b, NODE_LEN));
  EXPECT_EQ(0, MEMCMP(a, b, NODE_LEN));
  EXPECT_EQ(0x01, a[0] & 0x01);

  ObUUIDNode n1 = make_uninited_node();
  ObUUIDNode n2 = make_uninited_node();
  ASSERT_EQ(OB_SUCCESS, n1.init(mac_not_found, ObUUIDNode::get_process_random_node));
  ASSERT_EQ(OB_SUCCESS, n2.init(mac_not_found, ObUUIDNode::get_process_random_node));
  EXPECT_EQ(0, MEMCMP(a, n1.mac_addr_, NODE_LEN));
  EXPECT_EQ(0, MEMCMP(a, n2.mac_addr_, NODE_LEN));
}

TEST_F(TestExprUuidNode, read_fully_handles_partial_reads)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));
  const unsigned char data[NODE_LEN] = {1, 2, 3, 4, 5, 6};
  ASSERT_EQ(2, write(fds[1], data, 2));
  ASSERT_EQ(1, write(fds[1], data + 2, 1));
  ASSERT_EQ(3, write(fds[1], data + 3, 3));
  unsigned char out[NODE_LEN] = {0};
  EXPECT_EQ(OB_SUCCESS, ObUUIDNode::read_fully(fds[0], out, NODE_LEN));
  EXPECT_EQ(0, MEMCMP(data, out, NODE_LEN));
  close(fds[0]);
  close(fds[1]);
}

TEST_F(TestExprUuidNode, read_fully_rejects_short_input)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));
  const unsigned char data[3] = {1, 2, 3};
  ASSERT_EQ(3, write(fds[1], data, sizeof(data)));
  close(fds[1]);
  unsigned char out[NODE_LEN] = {0};
  EXPECT_NE(OB_SUCCESS, ObUUIDNode::read_fully(fds[0], out, NODE_LEN));
  close(fds[0]);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObUUIDNode::read_fully(-1, out, NODE_LEN));
}

TEST_F(TestExprUuidNode, default_init_succeeds_on_host)
{
  ObUUIDNode node = make_uninited_node();
  ASSERT_EQ(OB_SUCCESS, node.init());
  EXPECT_TRUE(node.is_inited_);
  const unsigned char zero[NODE_LEN] = {0};
  EXPECT_NE(0, MEMCMP(zero, node.mac_addr_, NODE_LEN));
}

TEST_F(TestExprUuidNode, gen_server_uuid_returns_version1_uuid)
{
  char uuid[UuidCommon::LENGTH_UUID + 1] = {0};
  ASSERT_EQ(OB_SUCCESS, ObExprUuid::gen_server_uuid(uuid, UuidCommon::LENGTH_UUID));
  EXPECT_EQ('-', uuid[8]);
  EXPECT_EQ('-', uuid[13]);
  EXPECT_EQ('1', uuid[14]);
  EXPECT_EQ('-', uuid[18]);
  EXPECT_EQ('-', uuid[23]);
}

TEST_F(TestExprUuidNode, linux_entropy_falls_back_when_getrandom_is_denied)
{
  FallbackFile file(FALLBACK_BYTES, NODE_LEN);
  ASSERT_TRUE(file.ok());
  unsigned char out[NODE_LEN] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::get_linux_entropy(getrandom_eperm, file.path(), out, NODE_LEN));
  EXPECT_EQ(0, MEMCMP(FALLBACK_BYTES, out, NODE_LEN));
  EXPECT_EQ(1, getrandom_calls);
}

TEST_F(TestExprUuidNode, linux_entropy_falls_back_when_getrandom_is_missing)
{
  FallbackFile file(FALLBACK_BYTES, NODE_LEN);
  ASSERT_TRUE(file.ok());
  unsigned char out[NODE_LEN] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::get_linux_entropy(getrandom_enosys, file.path(), out, NODE_LEN));
  EXPECT_EQ(0, MEMCMP(FALLBACK_BYTES, out, NODE_LEN));
}

TEST_F(TestExprUuidNode, linux_entropy_retries_eintr_and_partial_results)
{
  unsigned char out[NODE_LEN] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::get_linux_entropy(getrandom_interrupted_partial, "/nonexistent",
                                                      out, NODE_LEN));
  EXPECT_EQ(1 + NODE_LEN, getrandom_calls);
  for (int64_t i = 0; i < NODE_LEN; ++i) {
    EXPECT_EQ(0xa0 + i + 2, out[i]);
  }
}

TEST_F(TestExprUuidNode, linux_entropy_reports_other_errors)
{
  FallbackFile file(FALLBACK_BYTES, NODE_LEN);
  ASSERT_TRUE(file.ok());
  unsigned char out[NODE_LEN] = {0};
  EXPECT_EQ(OB_ERR_SYS, ObUUIDNode::get_linux_entropy(getrandom_eio, file.path(), out, NODE_LEN));
}

TEST_F(TestExprUuidNode, linux_entropy_fails_when_fallback_is_unusable)
{
  unsigned char out[NODE_LEN] = {0};
  EXPECT_EQ(OB_ERR_SYS, ObUUIDNode::get_linux_entropy(getrandom_eperm, "/nonexistent/urandom",
                                                      out, NODE_LEN));
  FallbackFile short_file(FALLBACK_BYTES, 3);
  ASSERT_TRUE(short_file.ok());
  EXPECT_NE(OB_SUCCESS, ObUUIDNode::get_linux_entropy(getrandom_eperm, short_file.path(),
                                                      out, NODE_LEN));
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObUUIDNode::get_linux_entropy(nullptr, "/dev/urandom", out, NODE_LEN));
}

TEST_F(TestExprUuidNode, gen_server_uuid_keeps_node_stable)
{
  char first[UuidCommon::LENGTH_UUID + 1] = {0};
  char second[UuidCommon::LENGTH_UUID + 1] = {0};
  ASSERT_EQ(OB_SUCCESS, ObExprUuid::gen_server_uuid(first, UuidCommon::LENGTH_UUID));
  ASSERT_EQ(OB_SUCCESS, ObExprUuid::gen_server_uuid(second, UuidCommon::LENGTH_UUID));
  // the last 12 hex digits encode the node
  EXPECT_EQ(0, MEMCMP(first + 24, second + 24, 12));
  EXPECT_NE(0, MEMCMP(first, second, UuidCommon::LENGTH_UUID));

  ObUUIDNode node = make_uninited_node();
  ASSERT_EQ(OB_SUCCESS, node.init());
  char expected[13] = {0};
  for (int64_t i = 0; i < NODE_LEN; ++i) {
    snprintf(expected + 2 * i, 3, "%02x", node.mac_addr_[i]);
  }
  EXPECT_EQ(0, MEMCMP(expected, first + 24, 12));
}

}  // namespace sql
}  // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
