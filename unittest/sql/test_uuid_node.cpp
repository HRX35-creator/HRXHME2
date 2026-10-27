/*
 * Copyright (c) 2026 OceanBase.
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

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <gtest/gtest.h>

#include "sql/engine/expr/ob_uuid_node.h"

namespace
{

constexpr int SOCKET_FD = 19001;
constexpr int ENTROPY_FD = 19002;
const std::array<unsigned char, 6> MAC = {{0x24, 0x35, 0x46, 0x57, 0x68, 0x79}};
const std::array<unsigned char, 6> RANDOM_BYTES = {{0xa4, 0x13, 0x22, 0x31, 0x40, 0x5f}};
const std::array<unsigned char, 6> SENTINEL = {{0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc}};

struct Interface
{
  const char *name;
  short flags;
  int flags_error;
  int hardware_error;
};

struct ReadResult
{
  ssize_t count;
  int error;
};

// GNU --wrap intercepts the production helper's real POSIX calls. Other
// descriptors and paths still use libc, including calls made by GoogleTest.
struct Syscalls
{
  bool active = false;
  int socket_error = 0;
  int enumeration_error = 0;
  int socket_calls = 0;
  int open_calls = 0;
  int entropy_opens = 0;
  int read_calls = 0;
  int socket_closes = 0;
  int entropy_closes = 0;
  int open_flags = 0;
  size_t random_offset = 0;
  std::array<unsigned char, 6> mac = MAC;
  unsigned short hardware_family = ARPHRD_ETHER;
  std::vector<Interface> interfaces = {{"eth0", IFF_UP, 0, 0}};
  std::deque<int> open_errors;
  std::deque<ReadResult> read_results;
};

Syscalls calls;

const Interface *find_interface(const char *name)
{
  for (const auto &interface : calls.interfaces) {
    if (0 == std::strncmp(name, interface.name, IFNAMSIZ)) {
      return &interface;
    }
  }
  return nullptr;
}

class UuidNode : public ::testing::Test
{
protected:
  void SetUp() override
  {
    calls = Syscalls();
    calls.active = true;
  }

  void TearDown() override
  {
    calls.active = false;
    EXPECT_EQ(0 == calls.socket_error ? calls.socket_calls : 0, calls.socket_closes);
    EXPECT_EQ(calls.entropy_opens, calls.entropy_closes);
  }

  void expect_random(const std::array<unsigned char, 6> &node,
                     const oceanbase::sql::uuid_node::NodeDiagnostics &diagnostics)
  {
    auto expected = RANDOM_BYTES;
    expected[0] |= 0x01;
    EXPECT_EQ(expected, node);
    EXPECT_TRUE(diagnostics.used_random);
    EXPECT_EQ(1, node[0] & 0x01);
    EXPECT_EQ(nullptr, diagnostics.entropy_operation);
    EXPECT_EQ(0, diagnostics.entropy_errno);
    EXPECT_NE(0, calls.open_flags & O_CLOEXEC);
    EXPECT_EQ(1, calls.entropy_closes);
  }
};

using oceanbase::sql::uuid_node::NodeDiagnostics;
using oceanbase::sql::uuid_node::generate_random_node;
using oceanbase::sql::uuid_node::initialize_linux_node;

TEST_F(UuidNode, successful_mac_does_not_open_entropy)
{
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(MAC, node);
  EXPECT_FALSE(diagnostics.used_random);
  EXPECT_EQ(nullptr, diagnostics.discovery_operation);
  EXPECT_EQ(0, calls.open_calls);
  EXPECT_EQ(0, calls.read_calls);
  EXPECT_EQ(1, calls.socket_closes);
}

TEST_F(UuidNode, skips_loopback_before_usable_mac)
{
  calls.interfaces = {{"lo", IFF_LOOPBACK | IFF_UP, 0, 0}, {"eth0", IFF_UP, 0, 0}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(MAC, node);
  EXPECT_EQ(0, calls.open_calls);
  EXPECT_EQ(1, calls.socket_closes);
}

TEST_F(UuidNode, empty_interface_list_uses_random_node)
{
  calls.interfaces.clear();
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("no usable MAC address", diagnostics.discovery_operation);
  EXPECT_EQ(1, calls.socket_closes);
}

TEST_F(UuidNode, loopback_only_uses_random_node)
{
  calls.interfaces = {{"lo", IFF_LOOPBACK | IFF_UP, 0, 0}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("no usable MAC address", diagnostics.discovery_operation);
}

TEST_F(UuidNode, all_zero_hardware_address_uses_random_node)
{
  calls.mac.fill(0);
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("no usable MAC address", diagnostics.discovery_operation);
}

TEST_F(UuidNode, non_ethernet_hardware_address_uses_random_node)
{
  calls.hardware_family = ARPHRD_INFINIBAND;
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("no usable MAC address", diagnostics.discovery_operation);
}

TEST_F(UuidNode, socket_permission_error_uses_random_node)
{
  calls.socket_error = EPERM;
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("socket", diagnostics.discovery_operation);
  EXPECT_EQ(EPERM, diagnostics.discovery_errno);
  EXPECT_EQ(0, calls.socket_closes);
}

TEST_F(UuidNode, interface_enumeration_permission_error_uses_random_node)
{
  calls.enumeration_error = EPERM;
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("ioctl(SIOCGIFCONF)", diagnostics.discovery_operation);
  EXPECT_EQ(EPERM, diagnostics.discovery_errno);
  EXPECT_EQ(1, calls.socket_closes);
}

TEST_F(UuidNode, interface_flags_permission_error_uses_random_node)
{
  calls.interfaces[0].flags_error = EPERM;
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("ioctl(SIOCGIFFLAGS)", diagnostics.discovery_operation);
  EXPECT_EQ(EPERM, diagnostics.discovery_errno);
}

TEST_F(UuidNode, hardware_address_permission_error_uses_random_node)
{
  calls.interfaces[0].hardware_error = EPERM;
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_STREQ("ioctl(SIOCGIFHWADDR)", diagnostics.discovery_operation);
  EXPECT_EQ(EPERM, diagnostics.discovery_errno);
}

TEST_F(UuidNode, flags_failure_does_not_hide_later_usable_mac)
{
  calls.interfaces = {{"eth0", IFF_UP, EPERM, 0}, {"eth1", IFF_UP, 0, 0}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(MAC, node);
  EXPECT_FALSE(diagnostics.used_random);
  EXPECT_EQ(0, calls.open_calls);
}

TEST_F(UuidNode, hardware_failure_does_not_hide_later_usable_mac)
{
  calls.interfaces = {{"eth0", IFF_UP, 0, EPERM}, {"eth1", IFF_UP, 0, 0}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(MAC, node);
  EXPECT_FALSE(diagnostics.used_random);
  EXPECT_EQ(0, calls.open_calls);
}

TEST_F(UuidNode, entropy_open_retries_eintr)
{
  calls.interfaces.clear();
  calls.open_errors = {EINTR, EINTR};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_EQ(3, calls.open_calls);
}

TEST_F(UuidNode, entropy_short_reads_fill_the_entire_node)
{
  calls.interfaces.clear();
  calls.read_results = {{1, 0}, {2, 0}, {3, 0}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_EQ(3, calls.read_calls);
}

TEST_F(UuidNode, entropy_read_retries_eintr_after_a_short_read)
{
  calls.interfaces.clear();
  calls.read_results = {{-1, EINTR}, {2, 0}, {-1, EINTR}, {4, 0}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_EQ(4, calls.read_calls);
}

TEST_F(UuidNode, entropy_open_failure_does_not_publish_node)
{
  calls.socket_error = EPERM;
  calls.open_errors = {EACCES};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_FALSE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(SENTINEL, node);
  EXPECT_STREQ("socket", diagnostics.discovery_operation);
  EXPECT_EQ(EPERM, diagnostics.discovery_errno);
  EXPECT_STREQ("open(/dev/urandom)", diagnostics.entropy_operation);
  EXPECT_EQ(EACCES, diagnostics.entropy_errno);
  EXPECT_EQ(0, calls.read_calls);
  EXPECT_EQ(0, calls.entropy_closes);
}

TEST_F(UuidNode, entropy_read_failure_does_not_publish_partial_node)
{
  calls.interfaces.clear();
  calls.read_results = {{2, 0}, {-1, EIO}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_FALSE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(SENTINEL, node);
  EXPECT_STREQ("read(/dev/urandom)", diagnostics.entropy_operation);
  EXPECT_EQ(EIO, diagnostics.entropy_errno);
  EXPECT_EQ(1, calls.socket_closes);
  EXPECT_EQ(1, calls.entropy_closes);
}

TEST_F(UuidNode, entropy_eof_does_not_publish_partial_node)
{
  calls.interfaces.clear();
  calls.read_results = {{3, 0}, {0, 0}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_FALSE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(SENTINEL, node);
  EXPECT_STREQ("read(/dev/urandom)", diagnostics.entropy_operation);
  EXPECT_EQ(EIO, diagnostics.entropy_errno);
  EXPECT_EQ(1, calls.entropy_closes);
}

TEST_F(UuidNode, initialize_clears_stale_diagnostics)
{
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  diagnostics.discovery_operation = "old discovery error";
  diagnostics.discovery_errno = EIO;
  diagnostics.entropy_operation = "old entropy error";
  diagnostics.entropy_errno = EACCES;
  diagnostics.used_random = true;
  ASSERT_TRUE(initialize_linux_node(node.data(), diagnostics));
  EXPECT_EQ(MAC, node);
  EXPECT_EQ(nullptr, diagnostics.discovery_operation);
  EXPECT_EQ(0, diagnostics.discovery_errno);
  EXPECT_EQ(nullptr, diagnostics.entropy_operation);
  EXPECT_EQ(0, diagnostics.entropy_errno);
  EXPECT_FALSE(diagnostics.used_random);
}

TEST_F(UuidNode, shared_random_helper_sets_the_multicast_bit)
{
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_TRUE(generate_random_node(node.data(), diagnostics));
  expect_random(node, diagnostics);
  EXPECT_EQ(0, calls.socket_calls);
}

TEST_F(UuidNode, shared_random_helper_preserves_node_on_error)
{
  calls.read_results = {{2, 0}, {-1, EIO}};
  auto node = SENTINEL;
  NodeDiagnostics diagnostics;
  ASSERT_FALSE(generate_random_node(node.data(), diagnostics));
  EXPECT_EQ(SENTINEL, node);
  EXPECT_EQ(1, calls.entropy_closes);
}

}  // namespace

extern "C" {

int __real_socket(int domain, int type, int protocol);
int __real_ioctl(int fd, unsigned long request, ...);
int __real_open(const char *path, int flags, ...);
ssize_t __real_read(int fd, void *buffer, size_t count);
int __real_close(int fd);

int __wrap_socket(int domain, int type, int protocol)
{
  if (!calls.active || AF_INET != domain || SOCK_DGRAM != type || IPPROTO_IP != protocol) {
    return __real_socket(domain, type, protocol);
  }
  ++calls.socket_calls;
  if (0 != calls.socket_error) {
    errno = calls.socket_error;
    return -1;
  }
  return SOCKET_FD;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (!calls.active || SOCKET_FD != fd) {
    return __real_ioctl(fd, request, argument);
  }
  if (SIOCGIFCONF == request) {
    if (0 != calls.enumeration_error) {
      errno = calls.enumeration_error;
      return -1;
    }
    auto *configuration = static_cast<struct ifconf *>(argument);
    const size_t count = std::min(calls.interfaces.size(),
                                 static_cast<size_t>(configuration->ifc_len) / sizeof(struct ifreq));
    for (size_t i = 0; i < count; ++i) {
      struct ifreq &interface = configuration->ifc_req[i];
      std::memset(&interface, 0, sizeof(interface));
      std::strncpy(interface.ifr_name, calls.interfaces[i].name, IFNAMSIZ - 1);
    }
    configuration->ifc_len = static_cast<int>(count * sizeof(struct ifreq));
    return 0;
  }
  auto *interface_request = static_cast<struct ifreq *>(argument);
  const Interface *interface = find_interface(interface_request->ifr_name);
  if (nullptr == interface) {
    errno = ENODEV;
    return -1;
  }
  if (SIOCGIFFLAGS == request) {
    if (0 != interface->flags_error) {
      errno = interface->flags_error;
      return -1;
    }
    interface_request->ifr_flags = interface->flags;
    return 0;
  } else if (SIOCGIFHWADDR == request) {
    if (0 != interface->hardware_error) {
      errno = interface->hardware_error;
      return -1;
    }
    interface_request->ifr_hwaddr.sa_family = calls.hardware_family;
    std::memcpy(interface_request->ifr_hwaddr.sa_data, calls.mac.data(), calls.mac.size());
    return 0;
  }
  errno = EINVAL;
  return -1;
}

int __wrap_open(const char *path, int flags, ...)
{
  mode_t mode = 0;
  bool with_mode = 0 != (flags & O_CREAT);
#ifdef O_TMPFILE
  with_mode = with_mode || O_TMPFILE == (flags & O_TMPFILE);
#endif
  if (with_mode) {
    va_list arguments;
    va_start(arguments, flags);
    mode = va_arg(arguments, mode_t);
    va_end(arguments);
  }
  if (!calls.active || 0 != std::strcmp(path, "/dev/urandom")) {
    return with_mode ? __real_open(path, flags, mode) : __real_open(path, flags);
  }
  ++calls.open_calls;
  calls.open_flags = flags;
  if (!calls.open_errors.empty()) {
    const int error = calls.open_errors.front();
    calls.open_errors.pop_front();
    if (0 != error) {
      errno = error;
      return -1;
    }
  }
  ++calls.entropy_opens;
  return ENTROPY_FD;
}

ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
  if (!calls.active || ENTROPY_FD != fd) {
    return __real_read(fd, buffer, count);
  }
  ++calls.read_calls;
  ReadResult result = {static_cast<ssize_t>(count), 0};
  if (!calls.read_results.empty()) {
    result = calls.read_results.front();
    calls.read_results.pop_front();
  }
  if (result.count < 0) {
    errno = result.error;
    return -1;
  }
  const size_t available = RANDOM_BYTES.size() - calls.random_offset;
  const size_t copied = std::min(std::min(count, available), static_cast<size_t>(result.count));
  std::memcpy(buffer, RANDOM_BYTES.data() + calls.random_offset, copied);
  calls.random_offset += copied;
  return static_cast<ssize_t>(copied);
}

int __wrap_close(int fd)
{
  if (calls.active && SOCKET_FD == fd) {
    ++calls.socket_closes;
    return 0;
  } else if (calls.active && ENTROPY_FD == fd) {
    ++calls.entropy_closes;
    return 0;
  }
  return __real_close(fd);
}

}  // extern "C"

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
