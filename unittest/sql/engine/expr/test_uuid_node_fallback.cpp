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

// Run with run_uuid_node_fallback.py. uuid_production.inc is generated from the
// actual production method bodies, not a second implementation of UUID logic.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

using uchar = unsigned char;

namespace oceanbase { namespace common {
constexpr int OB_SUCCESS = 0;
constexpr int OB_ERR_UNEXPECTED = -4016;
constexpr int OB_ALLOCATE_MEMORY_FAILED = -4013;
using ObLength = int64_t;
using ObLatch = std::mutex;
struct ObLatchIds { static constexpr int DEFAULT_MUTEX = 0; };
struct ObLatchWGuard {
  ObLatchWGuard(ObLatch &lock, int) : guard_(lock) {}
  std::lock_guard<ObLatch> guard_;
};
struct ObArenaAllocator {
  explicit ObArenaAllocator(const char *) {}
  ~ObArenaAllocator() { for (void *p : allocations_) { std::free(p); } }
  void *alloc(size_t size) { void *p = std::malloc(size); allocations_.push_back(p); return p; }
  std::vector<void *> allocations_;
};
}}
using namespace oceanbase::common;

struct Field { std::string text; };
template<class T> Field field(const char *name, const T &value)
{
  std::ostringstream os;
  os << name << '=' << value;
  return {os.str()};
}
std::vector<std::string> logs;
void capture_log(const char *message, std::initializer_list<Field> fields)
{
  std::string text(message);
  for (const Field &item : fields) { text += " " + item.text; }
  logs.push_back(text);
}
#define K(value) field(#value, (value))
#define LOG_WARN(message, ...) capture_log(message, {__VA_ARGS__})
#define LOG_ERROR(message, ...) capture_log(message, {__VA_ARGS__})
#define OB_ISNULL(value) (nullptr == (value))
#define OB_UNLIKELY(value) (value)
#define OB_SUCC(value) (OB_SUCCESS == (value))
#define OB_FAIL(value) (OB_SUCCESS != (ret = (value)))
#define MEMCPY std::memcpy
#define DISALLOW_COPY_AND_ASSIGN(type)

enum class Discovery { MAC, EMPTY, LOOPBACK, SOCKET_DENIED, ENUM_DENIED, FLAGS_DENIED, HW_DENIED, LATER_MAC };
enum class Entropy { FULL, SHORT_INTERRUPTED, INTERRUPTED, UNSUPPORTED, DENIED, ZERO, PARTIAL_ERROR, REAL };
Discovery discovery = Discovery::MAC;
Entropy entropy = Entropy::FULL;
constexpr int SOCK_FD = 71;
const std::array<unsigned char, 6> MAC = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a};
const std::array<unsigned char, 6> RANDOM = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65};
int socket_calls = 0, socket_closes = 0, entropy_calls = 0;
size_t entropy_offset = 0;
bool fixed_clock = false;
timespec clock_value = {10, 100};

extern "C" ssize_t __real_getrandom(void *, size_t, unsigned int);
extern "C" int __real_close(int);
extern "C" int __real_clock_gettime(clockid_t, timespec *);

extern "C" int __wrap_socket(int domain, int type, int protocol)
{
  if (domain != AF_INET || type != SOCK_DGRAM || protocol != IPPROTO_IP) { std::abort(); }
  ++socket_calls;
  if (discovery == Discovery::SOCKET_DENIED) { errno = EPERM; return -1; }
  return SOCK_FD;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
  if (fd != SOCK_FD) { std::abort(); }
  va_list args;
  va_start(args, request);
  void *argument = va_arg(args, void *);
  va_end(args);
  if (request == SIOCGIFCONF) {
    if (discovery == Discovery::ENUM_DENIED) { errno = EPERM; return -1; }
    auto *config = static_cast<ifconf *>(argument);
    int count = discovery == Discovery::EMPTY ? 0 : (discovery == Discovery::LATER_MAC ? 2 : 1);
    std::memset(config->ifc_buf, 0, static_cast<size_t>(count) * sizeof(ifreq));
    for (int i = 0; i < count; ++i) {
      std::snprintf(config->ifc_req[i].ifr_name, IFNAMSIZ, "test%d", i);
    }
    config->ifc_len = count * static_cast<int>(sizeof(ifreq));
    return 0;
  }
  auto *iface = static_cast<ifreq *>(argument);
  if (request == SIOCGIFFLAGS) {
    if (discovery == Discovery::FLAGS_DENIED ||
        (discovery == Discovery::LATER_MAC && std::strcmp(iface->ifr_name, "test0") == 0)) {
      errno = EACCES; return -1;
    }
    iface->ifr_flags = discovery == Discovery::LOOPBACK ? IFF_LOOPBACK : 0;
    return 0;
  }
  if (request == SIOCGIFHWADDR) {
    if (discovery == Discovery::HW_DENIED) { errno = EPERM; return -1; }
    std::memcpy(iface->ifr_hwaddr.sa_data, MAC.data(), MAC.size());
    return 0;
  }
  std::abort();
}

extern "C" ssize_t __wrap_getrandom(void *buffer, size_t length, unsigned int flags)
{
  if (flags != 0) { std::abort(); }
  ++entropy_calls;
  if (entropy == Entropy::REAL) { return __real_getrandom(buffer, length, flags); }
  if (entropy == Entropy::UNSUPPORTED) { errno = ENOSYS; return -1; }
  if (entropy == Entropy::INTERRUPTED && entropy_calls == 1) { errno = EINTR; return -1; }
  if (entropy == Entropy::DENIED || (entropy == Entropy::PARTIAL_ERROR && entropy_calls > 1)) {
    errno = EACCES; return -1;
  }
  if (entropy == Entropy::ZERO) { return 0; }
  if (entropy == Entropy::SHORT_INTERRUPTED && entropy_calls == 2) { errno = EINTR; return -1; }
  size_t count = length;
  if (entropy == Entropy::SHORT_INTERRUPTED || entropy == Entropy::PARTIAL_ERROR) {
    count = std::min<size_t>(length, 2);
  }
  if (entropy_offset + count > RANDOM.size()) { std::abort(); }
  std::memcpy(buffer, RANDOM.data() + entropy_offset, count);
  entropy_offset += count;
  return static_cast<ssize_t>(count);
}

extern "C" int __wrap_close(int fd)
{
  if (fd == SOCK_FD) { ++socket_closes; }
  else { return __real_close(fd); }
  // Deliberately clobber errno: diagnostics must retain the discovery error.
  errno = EBADF;
  return 0;
}

extern "C" int __wrap_clock_gettime(clockid_t id, timespec *value)
{
  if (!fixed_clock) { return __real_clock_gettime(id, value); }
  *value = clock_value;
  return 0;
}

#include "uuid_production.inc"
using namespace oceanbase::sql;

void reset(Discovery d, Entropy e)
{
  discovery = d;
  entropy = e;
  socket_calls = socket_closes = entropy_calls = 0;
  entropy_offset = 0;
  logs.clear();
  fixed_clock = false;
}
void require(bool condition, const char *message)
{
  if (!condition) { throw std::runtime_error(message); }
}
bool logged(const std::string &fragment)
{
  return std::any_of(logs.begin(), logs.end(), [&](const std::string &line) {
    return line.find(fragment) != std::string::npos;
  });
}
int initialize(ObUUIDNode &node, Discovery d, Entropy e)
{
  reset(d, e);
  std::memset(node.mac_addr_, 0xa5, sizeof(node.mac_addr_));
  node.is_inited_ = true; // init must clear even a previous successful state on failure.
  return node.init();
}
void synthetic(Discovery d, Entropy e = Entropy::FULL)
{
  ObUUIDNode node;
  require(initialize(node, d, e) == OB_SUCCESS, "fallback must recover initialization");
  require(node.is_inited_, "successful fallback must publish initialized state");
  auto expected = RANDOM;
  expected[0] |= 1;
  require(std::equal(expected.begin(), expected.end(), node.mac_addr_), "node must contain all six entropy bytes and multicast bit");
  require(socket_closes == (d == Discovery::SOCKET_DENIED ? 0 : 1), "socket must close exactly once if opened");
  require(entropy_calls > 0, "fallback must use OS entropy");
}
void entropy_failure(Entropy e)
{
  ObUUIDNode node;
  require(initialize(node, Discovery::ENUM_DENIED, e) == OB_ERR_UNEXPECTED, "entropy failure must remain an initialization failure");
  require(!node.is_inited_, "failed entropy must not publish initialized state");
  require(std::all_of(std::begin(node.mac_addr_), std::end(node.mac_addr_), [](unsigned char c) { return c == 0xa5; }), "partial entropy must not alter the node");
  require(socket_closes == 1, "discovery socket must be closed on entropy failure");
  require(logged("discovery_step=SIOCGIFCONF") && logged("discovery_errno=1"), "fatal diagnostic must retain actual discovery operation and errno");
  require(logged(e == Entropy::ZERO ? "entropy_errno=5" : (e == Entropy::UNSUPPORTED ? "entropy_errno=38" : "entropy_errno=13")), "fatal diagnostic must distinguish entropy error");
}

int main(int argc, char **argv)
{
  if (argc == 2 && std::strcmp(argv[1], "--node-only") == 0) {
    reset(Discovery::ENUM_DENIED, Entropy::REAL);
    ObUUIDNode node;
    if (!node.is_inited_ || !(node.mac_addr_[0] & 1)) { return 1; }
    for (unsigned char byte : node.mac_addr_) { std::printf("%02x", byte); }
    std::puts("");
    return 0;
  }
  int passed = 0, failed = 0;
  auto run = [&](const char *name, const std::function<void()> &test) {
    reset(Discovery::MAC, Entropy::FULL);
    try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception &error) { ++failed; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
    ObExprUuid::uuid_node = nullptr;
  };
  run("normal MAC compatibility; no entropy dependency", [] {
    ObUUIDNode node;
    require(initialize(node, Discovery::MAC, Entropy::DENIED) == OB_SUCCESS, "available MAC must still succeed");
    require(node.is_inited_ && std::equal(MAC.begin(), MAC.end(), node.mac_addr_), "MAC bytes must remain unchanged");
    require(entropy_calls == 0 && socket_closes == 1, "normal MAC must not read entropy");
  });
  run("later usable interface wins over an earlier error", [] {
    ObUUIDNode node;
    require(initialize(node, Discovery::LATER_MAC, Entropy::DENIED) == OB_SUCCESS, "must continue discovery");
    require(std::equal(MAC.begin(), MAC.end(), node.mac_addr_) && entropy_calls == 0, "later MAC must win");
  });
  run("empty interface list", [] { synthetic(Discovery::EMPTY); });
  run("loopback only", [] { synthetic(Discovery::LOOPBACK); });
  run("socket EPERM", [] { synthetic(Discovery::SOCKET_DENIED); require(logged("discovery_step=socket"), "socket error source missing"); });
  run("SIOCGIFCONF EPERM and saved errno", [] {
    synthetic(Discovery::ENUM_DENIED);
    require(logged("discovery_step=SIOCGIFCONF") && logged("discovery_errno=1"), "cleanup must not replace EPERM with EBADF");
  });
  run("SIOCGIFFLAGS EACCES", [] { synthetic(Discovery::FLAGS_DENIED); require(logged("discovery_errno=13"), "flags errno missing"); });
  run("SIOCGIFHWADDR EPERM", [] { synthetic(Discovery::HW_DENIED); require(logged("discovery_step=SIOCGIFHWADDR"), "MAC ioctl source missing"); });
  run("partial reads and EINTR", [] { synthetic(Discovery::EMPTY, Entropy::SHORT_INTERRUPTED); require(entropy_calls == 4, "must resume partial entropy reads"); });
  run("interrupted getrandom retries", [] { synthetic(Discovery::ENUM_DENIED, Entropy::INTERRUPTED); require(entropy_calls == 2, "EINTR must retry"); });
  run("entropy getrandom unavailable", [] { entropy_failure(Entropy::UNSUPPORTED); });
  run("entropy getrandom denied", [] { entropy_failure(Entropy::DENIED); });
  run("zero-byte entropy read", [] { entropy_failure(Entropy::ZERO); });
  run("partial entropy followed by failure", [] { entropy_failure(Entropy::PARTIAL_ERROR); });
  run("retry after failed initialization", [] {
    ObUUIDNode node;
    require(initialize(node, Discovery::ENUM_DENIED, Entropy::PARTIAL_ERROR) != OB_SUCCESS && !node.is_inited_, "first init should fail");
    reset(Discovery::EMPTY, Entropy::FULL);
    require(node.init() == OB_SUCCESS && node.is_inited_, "failed init must be retryable");
  });
  run("UUIDv1 version, RFC variant and stable synthetic node", [] {
    ObUUIDNode node;
    require(initialize(node, Discovery::ENUM_DENIED, Entropy::FULL) == OB_SUCCESS, "init failed");
    ObExprUuid::uuid_node = &node;
    ObUUIDTime::clock_seq_ = 0;
    ObUUIDTime::lasttime_ = 0;
    for (int i = 0; i < 1000; ++i) {
      unsigned char uuid[16];
      require(ObExprUuid::calc(uuid) == OB_SUCCESS, "calc failed");
      require((uuid[6] >> 4) == 1 && (uuid[8] & 0xc0) == 0x80, "UUID version or variant changed");
      require(std::equal(node.mac_addr_, node.mac_addr_ + 6, uuid + 10), "node changed across UUIDs");
    }
    require(entropy_calls == 1, "UUID generation must not reseed the node per UUID");
  });
  run("concurrent UUID uniqueness with real clock", [] {
    ObUUIDNode node;
    require(initialize(node, Discovery::ENUM_DENIED, Entropy::REAL) == OB_SUCCESS, "real entropy init failed");
    ObExprUuid::uuid_node = &node;
    ObUUIDTime::clock_seq_ = 0;
    ObUUIDTime::lasttime_ = 0;
    std::mutex output_lock;
    std::set<std::array<unsigned char, 16>> ids;
    bool ok = true;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
      threads.emplace_back([&] {
        for (int i = 0; i < 2048; ++i) {
          std::array<unsigned char, 16> uuid;
          const int ret = ObExprUuid::calc(uuid.data());
          std::lock_guard<std::mutex> guard(output_lock);
          ok = ok && ret == OB_SUCCESS;
          ids.insert(uuid);
        }
      });
    }
    for (auto &thread : threads) { thread.join(); }
    require(ok && ids.size() == 8192, "duplicate or failed concurrent UUID generation");
  });
  run("timestamp rollback and sequence rollover unchanged", [] {
    ObUUIDNode node;
    require(initialize(node, Discovery::EMPTY, Entropy::FULL) == OB_SUCCESS, "init failed");
    ObExprUuid::uuid_node = &node;
    fixed_clock = true;
    clock_value = {10, 100};
    ObUUIDTime::lasttime_ = 0;
    ObUUIDTime::clock_seq_ = 0x8123;
    uint64_t first = 0, second = 0;
    uint16_t seq1 = 0, seq2 = 0;
    require(ObUUIDTime::get_time(first, seq1) == OB_SUCCESS && seq1 == 0x8123, "initial sequence changed");
    clock_value.tv_sec = 9;
    require(ObUUIDTime::get_time(second, seq2) == OB_SUCCESS && second < first && seq2 == 0x8124, "rollback sequence handling changed");
    ObUUIDTime::clock_seq_ = 0xbfff;
    require(ObUUIDTime::get_time(second, seq2) == OB_SUCCESS && seq2 == 0x8000, "rollover or variant changed");
  });
  run("temporary gen_server_uuid path and subsequent initialization", [] {
    ObExprUuid::uuid_node = nullptr;
    reset(Discovery::ENUM_DENIED, Entropy::REAL);
    ObUUIDTime::clock_seq_ = 0;
    char first[36], second[36];
    require(ObExprUuid::gen_server_uuid(first, sizeof(first)) == OB_SUCCESS, "temporary server UUID failed");
    require(ObExprUuid::uuid_node == nullptr, "temporary node pointer must be reset");
    require(ObExprUuid::gen_server_uuid(second, sizeof(second)) == OB_SUCCESS, "second temporary init failed");
    require(ObExprUuid::uuid_node == nullptr && std::memcmp(first, second, sizeof(first)) != 0, "temporary nodes must reinitialize safely");
    require(first[14] == '1' && (first[19] == '8' || first[19] == '9' || first[19] == 'a' || first[19] == 'b'), "server UUID format changed");
    reset(Discovery::ENUM_DENIED, Entropy::REAL);
    ObUUIDNode permanent;
    require(permanent.is_inited_, "permanent initialization after temporary path failed");
  });
  std::cout << "SUMMARY passed=" << passed << " failed=" << failed << '\n';
  return failed == 0 ? 0 : 1;
}
