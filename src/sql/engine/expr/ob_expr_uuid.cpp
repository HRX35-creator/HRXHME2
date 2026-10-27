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

#define USING_LOG_PREFIX  SQL_ENG
#ifdef __EMSCRIPTEN__
#include <unistd.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#ifndef CONST
#define CONST const
#endif
#include <iphlpapi.h>
#else
#include <errno.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#ifdef __APPLE__
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <sys/random.h>
#else
#include <fcntl.h>
#include <sys/syscall.h>
#ifdef __ANDROID__
#include <ifaddrs.h>
#include <linux/if_packet.h>
#endif
#endif
#endif
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/engine/ob_exec_context.h"
#include "lib/time/ob_time_utility.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

int ObBigEndian::put_uint16(unsigned char *b, uint16_t v)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(b)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("unexpected error. pointer is null", K(ret), K(b));
  } else {
    b[0] = static_cast<unsigned char>(v >> 8);
    b[1] = static_cast<unsigned char>(v);
  }
  return ret;
}
int ObBigEndian::put_uint32(unsigned char *b, uint32_t v)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(b)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("unexpected error. pointer is null", K(ret), K(b));
  } else {
    b[0] = static_cast<unsigned char>(v >> 24);
    b[1] = static_cast<unsigned char>(v >> 16);
    b[2] = static_cast<unsigned char>(v >> 8);
    b[3] = static_cast<unsigned char>(v);
  }
  return ret;
}

ObUUIDNode::ObUUIDNode()
{
  is_inited_ = false;
  (void)init();
}

ObLatch ObUUIDNode::random_node_lock_;
unsigned char ObUUIDNode::random_node_[ObUUIDNode::NODE_LENGTH] = {0};
bool ObUUIDNode::has_random_node_ = false;

/*
 * Resolve the UUID node: prefer the hardware MAC address and fall back to a
 * random multicast node (RFC 9562 section 6.10) when no usable MAC address can
 * be obtained, e.g. in sandboxes that deny interface enumeration.
 */
int ObUUIDNode::init()
{
  return init(get_mac_address, get_process_random_node);
}

int ObUUIDNode::init(NodeProvider get_mac, NodeProvider get_random_node)
{
  int ret = OB_SUCCESS;
  unsigned char node[NODE_LENGTH] = {0};
  is_inited_ = false;
  if (OB_ISNULL(get_mac) || OB_ISNULL(get_random_node)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid uuid node provider", K(ret));
  } else {
    int mac_ret = get_mac(node, NODE_LENGTH);
    if (OB_SUCCESS == mac_ret) {
      // use the hardware MAC address
    } else if (OB_FAIL(get_random_node(node, NODE_LENGTH))) {
      LOG_ERROR("no usable MAC address and failed to generate random uuid node",
                K(ret), K(mac_ret));
    } else {
      LOG_WARN_RET(mac_ret, "no usable MAC address for uuid node, use random multicast node instead",
                   K(mac_ret));
    }
  }
  if (OB_SUCC(ret)) {
    // publish the node only after it has been completely resolved
    MEMCPY(mac_addr_, node, NODE_LENGTH);
    is_inited_ = true;
  }
  return ret;
}

int ObUUIDNode::gen_random_node(EntropyProvider get_entropy, unsigned char *node, const int64_t len)
{
  int ret = OB_SUCCESS;
  unsigned char buf[NODE_LENGTH] = {0};
  if (OB_ISNULL(get_entropy) || OB_ISNULL(node) || OB_UNLIKELY(NODE_LENGTH != len)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(node), K(len));
  } else if (OB_FAIL(get_entropy(buf, NODE_LENGTH))) {
    LOG_WARN("failed to read random bytes for uuid node", K(ret));
  } else {
    // multicast bit marks the node as not being an IEEE 802 address
    buf[0] |= 0x01;
    MEMCPY(node, buf, NODE_LENGTH);
  }
  return ret;
}

int ObUUIDNode::get_process_random_node(unsigned char *node, const int64_t len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node) || OB_UNLIKELY(NODE_LENGTH != len)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(node), K(len));
  } else {
    // generate once per process so that every uuid node initialization,
    // including the temporary one in gen_server_uuid, uses the same node
    ObLatchWGuard guard(random_node_lock_, ObLatchIds::DEFAULT_MUTEX);
    if (!has_random_node_) {
      if (OB_FAIL(gen_random_node(get_os_entropy, random_node_, NODE_LENGTH))) {
      } else {
        has_random_node_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      MEMCPY(node, random_node_, NODE_LENGTH);
    }
  }
  return ret;
}

#if !defined(_WIN32)
int ObUUIDNode::read_fully(const int fd, unsigned char *buf, const int64_t len)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  if (OB_UNLIKELY(fd < 0) || OB_ISNULL(buf) || OB_UNLIKELY(len < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(fd), KP(buf), K(len));
  }
  while (OB_SUCC(ret) && pos < len) {
    ssize_t n = ::read(fd, buf + pos, static_cast<size_t>(len - pos));
    if (n > 0) {
      pos += n;
    } else if (0 == n) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected end of file", K(ret), K(fd), K(pos), K(len));
    } else if (EINTR != errno) {
      const int err = errno;
      ret = OB_ERR_SYS;
      LOG_WARN("read failed", K(ret), K(fd), K(pos), K(len), K(err));
    }
  }
  return ret;
}
#endif

/*
 * Fill buf with bytes from the operating system CSPRNG.
 */
int ObUUIDNode::get_os_entropy(unsigned char *buf, const int64_t len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf) || OB_UNLIKELY(len < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(buf), K(len));
  } else {
#if defined(_WIN32)
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("random uuid node is not supported on this platform", K(ret));
#elif defined(__EMSCRIPTEN__) || defined(__APPLE__)
    // getentropy is bounded to 256 bytes per call
    static const int64_t MAX_GETENTROPY_LEN = 256;
    for (int64_t pos = 0; OB_SUCC(ret) && pos < len; pos += MAX_GETENTROPY_LEN) {
      const int64_t n = (len - pos < MAX_GETENTROPY_LEN) ? (len - pos) : MAX_GETENTROPY_LEN;
      if (0 != getentropy(buf + pos, static_cast<size_t>(n))) {
        const int err = errno;
        ret = OB_ERR_SYS;
        LOG_WARN("getentropy failed", K(ret), K(err));
      }
    }
#else
    bool use_urandom = true;
#ifdef SYS_getrandom
    use_urandom = false;
    int64_t pos = 0;
    while (OB_SUCC(ret) && !use_urandom && pos < len) {
      long n = syscall(SYS_getrandom, buf + pos, static_cast<size_t>(len - pos), 0);
      if (n > 0) {
        pos += n;
      } else if (n < 0 && EINTR == errno) {
        // retry
      } else if (n < 0 && ENOSYS == errno) {
        // kernel without getrandom(2)
        use_urandom = true;
      } else {
        const int err = errno;
        ret = OB_ERR_SYS;
        LOG_WARN("getrandom failed", K(ret), K(n), K(pos), K(len), K(err));
      }
    }
#endif
    if (OB_SUCC(ret) && use_urandom) {
      int fd = -1;
      do {
        fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
      } while (fd < 0 && EINTR == errno);
      if (fd < 0) {
        const int err = errno;
        ret = OB_ERR_SYS;
        LOG_WARN("failed to open /dev/urandom", K(ret), K(err));
      } else {
        if (OB_FAIL(read_fully(fd, buf, len))) {
          LOG_WARN("failed to read /dev/urandom", K(ret));
        }
        ::close(fd);
      }
    }
#endif
  }
  return ret;
}

static inline bool is_usable_mac_address(const unsigned char *mac, const int64_t len)
{
  bool usable = false;
  for (int64_t i = 0; i < len && !usable; ++i) {
    usable = (0 != mac[i]);
  }
  return usable;
}

/*
 * Get the MAC address of the first non-loopback interface.
 * Returns OB_ENTRY_NOT_EXIST if no usable MAC address is found, or OB_ERR_SYS
 * if interface enumeration fails.
 */
int ObUUIDNode::get_mac_address(unsigned char *node, const int64_t len)
{
  int ret = OB_SUCCESS;
  bool mac_addr_found = false;
  if (OB_ISNULL(node) || OB_UNLIKELY(NODE_LENGTH != len)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(node), K(len));
  } else {
#ifdef __EMSCRIPTEN__
  // Browsers expose no hardware MAC.
  ret = OB_NOT_SUPPORTED;
#elif defined(_WIN32)
  ULONG buf_len = 15000;
  PIP_ADAPTER_ADDRESSES addresses = NULL;
  addresses = (IP_ADAPTER_ADDRESSES *)malloc(buf_len);
  if (OB_ISNULL(addresses)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate adapter addresses", K(ret));
  } else {
    ULONG err = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buf_len);
    if (NO_ERROR != err) {
      ret = OB_ERR_SYS;
      LOG_WARN("GetAdaptersAddresses failed", K(ret), K(err));
    } else {
      for (PIP_ADAPTER_ADDRESSES addr = addresses; addr && !mac_addr_found; addr = addr->Next) {
        if (addr->PhysicalAddressLength == NODE_LENGTH && addr->IfType != IF_TYPE_SOFTWARE_LOOPBACK
            && is_usable_mac_address(addr->PhysicalAddress, NODE_LENGTH)) {
          MEMCPY(node, addr->PhysicalAddress, NODE_LENGTH);
          mac_addr_found = true;
        }
      }
    }
    free(addresses);
  }
#elif defined(__APPLE__) || defined(__ANDROID__)
  // Use getifaddrs: AF_LINK + sockaddr_dl on macOS, AF_PACKET + sockaddr_ll on Android
  struct ifaddrs *ifaddrs_list = nullptr;
  if (getifaddrs(&ifaddrs_list) != 0) {
    const int err = errno;
    ret = OB_ERR_SYS;
    LOG_WARN("getifaddrs failed", K(ret), K(err));
  } else {
    for (struct ifaddrs *ifa = ifaddrs_list; ifa != nullptr && !mac_addr_found; ifa = ifa->ifa_next) {
      const unsigned char *mac = nullptr;
      if (ifa->ifa_addr == nullptr || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
        continue;
      }
#if defined(__APPLE__)
      if (ifa->ifa_addr->sa_family == AF_LINK) {
        struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
        if (sdl->sdl_alen == NODE_LENGTH) {
          mac = reinterpret_cast<const unsigned char *>(LLADDR(sdl));
        }
      }
#else
      if (ifa->ifa_addr->sa_family == AF_PACKET) {
        struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;
        if (sll->sll_halen == NODE_LENGTH) {
          mac = sll->sll_addr;
        }
      }
#endif
      if (mac != nullptr && is_usable_mac_address(mac, NODE_LENGTH)) {
        MEMCPY(node, mac, NODE_LENGTH);
        mac_addr_found = true;
      }
    }
    freeifaddrs(ifaddrs_list);
  }
#else
  // Linux: Use ioctl to get MAC address
  struct ifreq ifr;
  struct ifconf ifc;
  char buf[1024];
  int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_IP);
  if (sock == -1) {
    const int err = errno;
    ret = OB_ERR_SYS;
    LOG_WARN("socket failed", K(ret), K(err));
  } else {
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
      const int err = errno;
      ret = OB_ERR_SYS;
      LOG_WARN("ioctl failed, SIOCGIFCONF", K(ret), K(err));
    } else {
      ifreq *it = ifc.ifc_req;
      ifreq *end = it + (ifc.ifc_len / sizeof(ifreq));
      for (; it != end && !mac_addr_found; ++it) {
        MEMSET(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, it->ifr_name, IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
          const int err = errno;
          LOG_WARN("ioctl failed, SIOCGIFFLAGS", K(err), K(ifr.ifr_name));
        } else if (ifr.ifr_flags & IFF_LOOPBACK) {
          // skip loopback interface
        } else if (ioctl(sock, SIOCGIFHWADDR, &ifr) != 0) {
          const int err = errno;
          LOG_WARN("ioctl failed, SIOCGIFHWADDR", K(err), K(ifr.ifr_name));
        } else if (is_usable_mac_address(reinterpret_cast<const unsigned char *>(ifr.ifr_hwaddr.sa_data),
                                         NODE_LENGTH)) {
          MEMCPY(node, ifr.ifr_hwaddr.sa_data, NODE_LENGTH);
          mac_addr_found = true;
        }
      }
    }
    close(sock);
  }
#endif
  }
  if (OB_SUCC(ret) && !mac_addr_found) {
    ret = OB_ENTRY_NOT_EXIST;
    LOG_WARN("no usable MAC address found", K(ret));
  }
  return ret;
}

int ObUUIDTime::get_time(uint64_t &time, uint16_t &seq)
{
  int ret = OB_SUCCESS;
  ObLatchWGuard guard(lock_, ObLatchIds::DEFAULT_MUTEX);
  uint64_t now = 0;
  if (OB_FAIL(time_now(now))) {
  } else {
    if (clock_seq_ == 0) {
      reset_clock_seq();
    }
    now = (now / uint64_t(100)) + G1582NS100;
    if (now <= lasttime_) {
      clock_seq_ = static_cast<uint16_t>(((clock_seq_ + 1) & 0x3fff) | 0x8000);
    }
    lasttime_ = now;
    time = now;
    seq = clock_seq_;
  }
  return ret;
}

int ObUUIDTime::time_now(uint64_t &now)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  now = static_cast<uint64_t>(ObTimeUtility::current_time_ns());
#else
  struct timespec ts;
  int tmpret = clock_gettime(CLOCK_REALTIME, &ts);
  if (tmpret != 0) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    now = uint64_t(ts.tv_sec) * uint64_t(1000000000) + uint64_t(ts.tv_nsec);
  }
#endif
  return ret;
}

void ObUUIDTime::reset_clock_seq()
{
  uint8_t b[2] = {0, 0};
#ifdef _WIN32
  srand(static_cast<unsigned int>(time(NULL)));
  b[0] = static_cast<uint8_t>(rand());
  b[1] = static_cast<uint8_t>(rand());
#else
  srandom(static_cast<unsigned int>(time(NULL)));
  b[0] = static_cast<uint8_t>(random());
  b[1] = static_cast<uint8_t>(random());
#endif
  uint16_t seq = static_cast<uint16_t>(uint16_t(b[0] << 8) | uint16_t(b[1]));
  clock_seq_ = static_cast<uint16_t>((seq & 0x3fff) | 0x8000);
}

int UuidCommon::bin2uuid(char *result, const uchar *src) {
  int ret = OB_SUCCESS;
  if (OB_ISNULL(result) || OB_ISNULL(src)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t pos = 0;
    const char *HEXCHARS = "0123456789abcdef";
    for (int64_t i = 0; pos < UuidCommon::LENGTH_UUID && i < 16; ++i) {
      if (4 == i || 6 == i || 8 == i || 10 == i) {
        result[pos++] = '-';
      }
      result[pos++] = HEXCHARS[src[i] >> 4 & 0xF];
      result[pos++] = HEXCHARS[src[i] & 0xF];
    }
  }
  return ret;
}

int UuidCommon::read_section(char *res, bool &is_valid, const char *src, int64_t len) {
  int ret = OB_SUCCESS;
  if (OB_ISNULL(res) || OB_ISNULL(src)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int cnt = 0;
    is_valid = true;
    while(is_valid && cnt < len) {
      if (!isxdigit(src[cnt * 2]) || !isxdigit(src[cnt * 2 + 1])) {
        is_valid = false;
      } else {
        char hi = common::get_xdigit(src[cnt * 2]);
        char lo = common::get_xdigit(src[cnt * 2 + 1]);
        res[cnt] = static_cast<uchar> ((hi << 4) + lo);
        cnt ++;
      }
    }
  }
  return ret;
}

//convert uuid to bin, is_valid is set to false if src is null or not a valid string-format UUID.
int UuidCommon::uuid2bin(char *result, bool &is_valid, const char *src, int64_t len) {
  int ret = OB_SUCCESS;
  is_valid = true;
  if (OB_ISNULL(result)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(src)) {
    is_valid = false;
  } else {
    char bin_buf[UuidCommon::BYTE_LENGTH + 1];
    switch (len) {
      case UuidCommon::LENGTH_UUID - 4:{
        if (OB_FAIL(UuidCommon::read_section(result, is_valid, src, UuidCommon::BYTE_LENGTH))) {
        }
      }
      break;
      case UuidCommon::LENGTH_UUID + 2: {
        if ('{' == src[0] && '}' == src[len - 1]) {
          src++;
        } else {
          is_valid = false;
        }
      }
      case UuidCommon::LENGTH_UUID: {
        int cnt = 0;
        for (int i = 0; OB_SUCC(ret) && i < 5 && is_valid; ++i) {
          if (OB_FAIL(UuidCommon::read_section(result + cnt, is_valid, src + cnt * 2 + i, UuidCommon::bytes_per_section[i]))) {
          } else {
            cnt += UuidCommon::bytes_per_section[i];
          }
          if (i < 4 && src[cnt * 2 + i] != '-') {
            is_valid = false;
          }
        }
      }
      break;
      default: {
        is_valid = false;
      }
    }
  }
  return ret;
}

ObExprUuid::ObExprUuid(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_SYS_UUID, N_UUID, 0, NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

ObExprUuid::ObExprUuid(
  ObIAllocator &alloc,
  ObExprOperatorType type,
  const char *name,
  int32_t param_num,
  int32_t dimension)
  : ObFuncExprOperator(alloc, type, name, param_num, NOT_VALID_FOR_GENERATED_COL, dimension)
{
}

ObExprUuid::~ObExprUuid()
{
}

int ObExprUuid::init()
{
  //will be inited once when and only when server starts
  //so, please do not worry too much about the contention even we use ob_malloc here.
  int ret = OB_SUCCESS;
  ObMemAttr mem_attr;
  mem_attr.label_ = ObModIds::OB_SQL_EXPR;
  uuid_node = static_cast<ObUUIDNode*>(ob_malloc(sizeof(ObUUIDNode), mem_attr));
  if (OB_UNLIKELY(NULL == uuid_node)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("allocate memory failed", K(ret));
  } else if (OB_FAIL(uuid_node->init())) {
    ob_free(uuid_node);//very important ! do not forget this !
    uuid_node = NULL;//very important ! do not forget this !
  }
  return ret;
}

int ObExprUuid::calc(unsigned char *scratch)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(scratch)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("unexpected error. pointer is null", K(ret), K(scratch));
  } else if (OB_ISNULL(uuid_node) || OB_UNLIKELY(!(uuid_node->is_inited_))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("unexpected error. uuid node is not inited", K(ret), K(uuid_node));
  } else {
    unsigned char *mac_addr = uuid_node->mac_addr_;
    uint64_t time = 0;
    uint16_t seq = 0;
    if (OB_FAIL(ObUUIDTime::get_time(time, seq))) {
    } else {
      uint32_t time_low = static_cast<uint32_t>(time);
      uint16_t time_mid = static_cast<uint16_t>(time >> 32);
      uint16_t time_hi = static_cast<uint16_t>((time >> 48) & 0x0fff);
      time_hi |= 0x1000;
      /*faint. really ugly. 4 if(ob_fail)s here, not efficient at all*/
      /*may I omit the tests for NULL on scratch ? */
      if (OB_FAIL(ObBigEndian::put_uint32(scratch, time_low))) {
      } else if (OB_FAIL(ObBigEndian::put_uint16(scratch + 4, time_mid))) {
      } else if (OB_FAIL(ObBigEndian::put_uint16(scratch + 6, time_hi))) {
      } else if (OB_FAIL(ObBigEndian::put_uint16(scratch + 8, seq))) {
      } else {
        MEMCPY(scratch + 10, mac_addr, 6);
      }
    }
  }
  return ret;
}

uint64_t ObUUIDTime::lasttime_ = 0;
uint16_t ObUUIDTime::clock_seq_ = 0;
ObLatch ObUUIDTime::lock_;

ObUUIDNode *ObExprUuid::uuid_node = NULL;

int ObExprUuid::eval_uuid(const ObExpr &expr, ObEvalCtx &ctx,
    ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;
  UNUSED(expr);
  unsigned char scratch[UuidCommon::LENGTH_UUID] = {0};
  if (OB_FAIL(calc(scratch))) {
  } else {
    char *buf = expr.get_str_res_mem(ctx, UuidCommon::LENGTH_UUID);
    int64_t pos = 0;
    if (OB_ISNULL(buf)) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "buff is null", K(ret));
    } else if (OB_FAIL(UuidCommon::bin2uuid(buf, scratch))) {
    } else {
      expr_datum.set_string(buf, UuidCommon::LENGTH_UUID);
    }
  }
  return ret;
}

int ObExprUuid::cg_expr(ObExprCGCtx &op_cg_ctx, const ObRawExpr &raw_expr,
    ObExpr &rt_expr) const
{
  UNUSED(raw_expr);
  UNUSED(op_cg_ctx);
  rt_expr.eval_func_ = ObExprUuid::eval_uuid;
  return OB_SUCCESS;
}

int ObExprUuid::gen_server_uuid(char *server_uuid, const int64_t uuid_len)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator calc_buf("ServerUuid");
  unsigned char scratch[UuidCommon::LENGTH_UUID] = {0};
  bool need_reset = false;
  if (OB_ISNULL(uuid_node)) {
    need_reset = true;
    if (OB_ISNULL(uuid_node = static_cast<ObUUIDNode*>(calc_buf.alloc(sizeof(ObUUIDNode))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(uuid_node->init())) {
    }
  }
  if (OB_FAIL(ret)) {
    //do nothing
  } else if (OB_ISNULL(server_uuid) || OB_UNLIKELY(uuid_len != UuidCommon::LENGTH_UUID)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(calc(scratch))) {
  } else if (OB_FAIL(UuidCommon::bin2uuid(server_uuid, scratch))) {
  } else {/*do nothing*/}

  if (need_reset) {
    uuid_node = NULL;
  }
  return ret;
}

ObExprUuid2bin::ObExprUuid2bin(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_SYS_UUID2BIN, N_UUID_TO_BIN, ONE_OR_TWO, VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

ObExprUuid2bin::~ObExprUuid2bin()
{
}


int ObExprUuid2bin::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t type_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  UNUSED(type_ctx);
  if (OB_UNLIKELY(NULL == types || type_num <= 0 || type_num > 2)) {
    ret = OB_ERR_PARAM_SIZE;
  } else {
    types[0].set_calc_type(common::ObVarcharType);
    if (2 == type_num) {
      types[1].set_calc_type(types[1].get_type());
    }
  }
  type.set_varbinary();
  type.set_length(UuidCommon::BYTE_LENGTH);
  type.set_collation_level(common::CS_LEVEL_COERCIBLE);
  return ret;
}

int ObExprUuid2bin::cg_expr(ObExprCGCtx &op_cg_ctx,
                          const ObRawExpr &raw_expr,
                          ObExpr &rt_expr) const
{
  UNUSED(op_cg_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(1 != rt_expr.arg_cnt_ && 2 != rt_expr.arg_cnt_)) {
    ret = OB_ERR_PARAM_SIZE;
  } else if (raw_expr.get_param_count() == 2) {
    const ObRawExpr *flag_expr = raw_expr.get_param_expr(1);
    if (OB_ISNULL(flag_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObObjType flag_expr_type = flag_expr->get_result_type().get_type();
      if (OB_UNLIKELY(ObNullType != flag_expr_type &&
                  !ob_is_integer_type(flag_expr_type))) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprUuid2bin::uuid2bin;
    // Only implement vectorization when parameter 0 is batch and parameter 1 is constant or null
    {
      OB_ASSERT(rt_expr.args_[0] != nullptr);
      if (rt_expr.args_[0]->is_batch_result()) {
        if (rt_expr.arg_cnt_ != 2 || !rt_expr.args_[1]->is_batch_result()) {
          rt_expr.eval_batch_func_ = ObExprUuid2bin::uuid2bin_batch;
        } else {
        }
      } else {
      }
    }
  }
  return ret;
}

int ObExprUuid2bin::uuid2bin(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;
  ObDatum *text = nullptr;
  ObDatum *swap_flag = nullptr;
  bool need_swap = false;
  bool is_null = false;
  if (OB_FAIL(expr.args_[0]->eval(ctx, text))) {
  } else if (OB_ISNULL(text)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (text->is_null()) {
    expr_datum.set_null();
    is_null = true;
  } else if (expr.arg_cnt_ == 2) {
    if (OB_ISNULL(expr.args_[1])) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr.args_[1]->eval(ctx, swap_flag))) {
    } else if (OB_ISNULL(swap_flag)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (swap_flag->is_null()) {
      need_swap = false;
    } else {
      need_swap = swap_flag->get_bool();
    }
  } else {
    need_swap = false;
  }

  if (OB_SUCC(ret) && !is_null) {
    ObString uuid_text = text->get_string();
    char *res_buf = expr.get_str_res_mem(ctx, UuidCommon::BYTE_LENGTH);
    const char *text_ptr = uuid_text.ptr();
    bool is_valid = true;
    
    if (OB_ISNULL(res_buf)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(UuidCommon::uuid2bin(res_buf, is_valid, text_ptr, uuid_text.length()))) {
    } else if (!is_valid) {
      ret = OB_ERR_INCORRECT_VALUE_FOR_FUNCTION;
      ObString string_type_str("string");
      ObString func_str(N_UUID_TO_BIN);
      LOG_USER_ERROR(OB_ERR_INCORRECT_VALUE_FOR_FUNCTION,
                      string_type_str.length(), string_type_str.ptr(),
                      uuid_text.length(), uuid_text.ptr(),
                      func_str.length(), func_str.ptr());
    } else if (need_swap) { //check if need swap
      char swap_buf[8];
      MEMCPY(swap_buf, res_buf + 6, 2);
      MEMCPY(swap_buf + 2, res_buf + 4, 2);
      MEMCPY(swap_buf + 4, res_buf, 4);
      MEMCPY(res_buf, swap_buf, 8);
    } else {
    }
    if (OB_SUCC(ret)) {
      expr_datum.set_string(res_buf, UuidCommon::BYTE_LENGTH);
    }
  }
  return ret;
}


int ObExprUuid2bin::uuid2bin_batch(const ObExpr &expr,
                                   ObEvalCtx &ctx,
                                   const ObBitVector &skip,
                                   const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  bool need_swap = false;
  if (OB_ISNULL(expr.args_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(expr.args_[0]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 2) {
    ObDatum *swap_flag = nullptr;
    if (OB_ISNULL(expr.args_[1])) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr.args_[1]->eval(ctx, swap_flag))) {
    } else if (OB_ISNULL(swap_flag)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (swap_flag->is_null()) {
      need_swap = false;
    } else {
      need_swap = swap_flag->get_bool();
    }
  } else {
    need_swap = false;
  }

  if (OB_SUCC(ret)) {
    ObDatum *res_datum = expr.locate_batch_datums(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum &text= expr.args_[0]->locate_expr_datum(ctx, i);
      if (text.is_null()) {
        res_datum[i].set_null();
      } else {
        ObString uuid_text = text.get_string();
        char *res_buf = expr.get_str_res_mem(ctx, UuidCommon::BYTE_LENGTH, i);
        const char *text_ptr = uuid_text.ptr();
        bool is_valid = true;
        if (OB_ISNULL(res_buf)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(UuidCommon::uuid2bin(res_buf, is_valid, text_ptr, uuid_text.length()))) {
        } else if (!is_valid) {
          ret = OB_ERR_INCORRECT_VALUE_FOR_FUNCTION;
          ObString string_type_str("string");
          ObString func_str(N_UUID_TO_BIN);
          LOG_USER_ERROR(OB_ERR_INCORRECT_VALUE_FOR_FUNCTION,
                        string_type_str.length(), string_type_str.ptr(),
                        uuid_text.length(), uuid_text.ptr(),
                        func_str.length(), func_str.ptr());
        } else if (need_swap) {
          char swap_buf[8];
          MEMCPY(swap_buf, res_buf + 6, 2);
          MEMCPY(swap_buf + 2, res_buf + 4, 2);
          MEMCPY(swap_buf + 4, res_buf, 4);
          MEMCPY(res_buf, swap_buf, 8);
        }
        if (OB_SUCC(ret)) {
          res_datum[i].set_string(res_buf, UuidCommon::BYTE_LENGTH);
        }
      }
      eval_flags.set(i);
    }
  }
  return ret;
}

ObExprIsUuid::ObExprIsUuid(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_SYS_IS_UUID, N_IS_UUID, 1, VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

ObExprIsUuid::~ObExprIsUuid()
{
}

int ObExprIsUuid::cg_expr(ObExprCGCtx &op_cg_ctx,
                          const ObRawExpr &raw_expr,
                          ObExpr &rt_expr) const
{
  UNUSED(op_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(1 != rt_expr.arg_cnt_)) {
    ret = OB_ERR_PARAM_SIZE;
  } else {
    rt_expr.eval_func_ = ObExprIsUuid::is_uuid;
    rt_expr.eval_batch_func_ = ObExprIsUuid::is_uuid_batch;
  }
  return ret;
}


int ObExprIsUuid::is_uuid(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;
  ObDatum *text = nullptr;
  if (OB_ISNULL(expr.args_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(expr.args_[0]->eval(ctx, text))) {
  } else if (OB_ISNULL(text)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (text->is_null()) {
    expr_datum.set_null();
  } else {
    ObString uuid_text = text->get_string();
    const char *text_ptr = uuid_text.ptr();
    char result_buf[UuidCommon::BYTE_LENGTH];
    bool is_valid = true;
    if (OB_ISNULL(text_ptr)) {
      expr_datum.set_bool(false);
    } else if (OB_FAIL(UuidCommon::uuid2bin(result_buf, is_valid, text_ptr, uuid_text.length()))) {
    } else {
      expr_datum.set_bool(is_valid);
    }
  }
  return ret;
}

int ObExprIsUuid::is_uuid_batch(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                const ObBitVector &skip,
                                const int64_t batch_size) {
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr.args_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(expr.args_[0]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObDatum *res_datum = expr.locate_batch_datums(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum &text= expr.args_[0]->locate_expr_datum(ctx, i);
      if (text.is_null()) {
        res_datum[i].set_null();
      } else {
        ObString uuid_text = text.get_string();
        const char *text_ptr = uuid_text.ptr();
        char result_buf[UuidCommon::BYTE_LENGTH];
        bool is_valid = true;
        if (OB_FAIL(UuidCommon::uuid2bin(result_buf, is_valid, text_ptr, uuid_text.length()))) {
        } else {
          res_datum[i].set_bool(is_valid);
        }
      }
      eval_flags.set(i);
    }
  }
  return ret;            
}

ObExprBin2uuid::ObExprBin2uuid(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_SYS_BIN2UUID, N_BIN_TO_UUID, ONE_OR_TWO, VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

ObExprBin2uuid::~ObExprBin2uuid()
{
}

int ObExprBin2uuid::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t type_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  UNUSED(type_ctx);
  if (OB_UNLIKELY(NULL == types || type_num <= 0 || type_num > 2)) {
    ret = OB_ERR_PARAM_SIZE;
  } else {
    types[0].set_calc_type(common::ObVarcharType);
    types[0].set_calc_collation_type(CS_TYPE_BINARY);
    if (2 == type_num) {
      types[1].set_calc_type(types[1].get_type());
    }
  }
  type.set_varchar();
  type.set_collation_level(common::CS_LEVEL_IMPLICIT);
  type.set_collation_type(common::ObCharset::get_default_collation(common::ObCharset::get_default_charset()));
  type.set_length(UuidCommon::LENGTH_UUID);
  return ret;
}

int ObExprBin2uuid::cg_expr(ObExprCGCtx &op_cg_ctx,
                          const ObRawExpr &raw_expr,
                          ObExpr &rt_expr) const
{
  UNUSED(op_cg_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(1 != rt_expr.arg_cnt_ && 2 != rt_expr.arg_cnt_)) {
    ret = OB_ERR_PARAM_SIZE;
  } else if (raw_expr.get_param_count() == 2) {
    const ObRawExpr *flag_expr = raw_expr.get_param_expr(1);
    if (OB_ISNULL(flag_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObObjType flag_expr_type = flag_expr->get_result_type().get_type();
      if (OB_UNLIKELY(ObNullType != flag_expr_type &&
                  !ob_is_integer_type(flag_expr_type))) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprBin2uuid::bin2uuid;
    // Only implement vectorization when parameter 0 is batch and parameter 1 is constant or null
    {
      OB_ASSERT(rt_expr.args_[0] != nullptr);
      if (rt_expr.args_[0]->is_batch_result()) {
        if (rt_expr.arg_cnt_ == 2) {
          {
            OB_ASSERT(rt_expr.args_[1] != nullptr);
            if (!rt_expr.args_[1]->is_batch_result()) {
              rt_expr.eval_batch_func_ = ObExprBin2uuid::bin2uuid_batch;
            }
          }
        } else {
          rt_expr.eval_batch_func_ = ObExprBin2uuid::bin2uuid_batch;
        }
      } else {
      }
    }
  }
  return ret;
}

int ObExprBin2uuid::bin2uuid(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;
  ObDatum *text = nullptr;
  ObDatum *swap_flag = nullptr;
  bool need_swap = false;
  bool is_null = false;
  if (OB_ISNULL(expr.args_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(expr.args_[0]->eval(ctx, text))) {
  } else if (OB_ISNULL(text)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (text->is_null()) {
    expr_datum.set_null();
    is_null = true;
  } else if (expr.arg_cnt_ == 2) {
    if (OB_ISNULL(expr.args_[1])) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr.args_[1]->eval(ctx, swap_flag))) {
    } else if (OB_ISNULL(swap_flag)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (swap_flag->is_null()) {
      need_swap = false;
    } else {
      need_swap = swap_flag->get_bool();
    }
  } else {
    need_swap = false;
  }

  if (OB_SUCC(ret) && !is_null) {
    ObString bin_text = text->get_string();
    char *res_buf = expr.get_str_res_mem(ctx, UuidCommon::LENGTH_UUID);
    const char *bin_text_ptr = bin_text.ptr();
    uchar new_bin_text[UuidCommon::BYTE_LENGTH];
    if (OB_ISNULL(res_buf)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(bin_text_ptr) || UuidCommon::BYTE_LENGTH != bin_text.length()) {
      ObString string_type_str("binary");
      ObString func_str(N_BIN_TO_UUID);
      int text_len = std::min(bin_text.length(), UuidCommon::BYTE_LENGTH * 2);
      SMART_VAR(char[UuidCommon::BYTE_LENGTH * 4], hex_buf) {
        int64_t pos = 0;
        if (OB_SUCC(common::hex_print(bin_text.ptr(), text_len, hex_buf, text_len*2, pos))) {
          ret = OB_ERR_INCORRECT_VALUE_FOR_FUNCTION;
          LOG_USER_ERROR(OB_ERR_INCORRECT_VALUE_FOR_FUNCTION,
                        string_type_str.length(), string_type_str.ptr(),
                        text_len*2, hex_buf,
                        func_str.length(), func_str.ptr());
        } else {
        }
      }
    } else  if (need_swap) {
      // The first 4 bytes are restored to "time-low".
      MEMCPY(new_bin_text, bin_text_ptr + 4, 4);
      // Bytes starting with 4th will be restored to "time-mid".
      MEMCPY(new_bin_text + 4, bin_text_ptr + 2, 2);
      // Bytes starting with 6th will be restored to "time-high".
      MEMCPY(new_bin_text + 6, bin_text_ptr, 2);
      // The last 8 bytes were not changed so we just copy them.
      MEMCPY(new_bin_text + 8, bin_text_ptr + 8, 8);
    } else {
      MEMCPY(new_bin_text, bin_text_ptr, UuidCommon::BYTE_LENGTH);
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(UuidCommon::bin2uuid(res_buf, new_bin_text))) {
      } else {
        expr_datum.set_string(res_buf, UuidCommon::LENGTH_UUID);
      }
    }
  }
  return ret;
}

int ObExprBin2uuid::bin2uuid_batch(const ObExpr &expr,
                                   ObEvalCtx &ctx,
                                   const ObBitVector &skip,
                                   const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  bool need_swap = false;
  if (OB_ISNULL(expr.args_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(expr.args_[0]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 2) {
    ObDatum *swap_flag = nullptr;
    if (OB_ISNULL(expr.args_[1])) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr.args_[1]->eval(ctx, swap_flag))) {
    } else if (OB_ISNULL(swap_flag)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (swap_flag->is_null()) {
      need_swap = false;
    } else {
      need_swap = swap_flag->get_bool();
    }
  } else {
    need_swap = false;
  }

  if (OB_SUCC(ret)) {
    ObDatum *res_datum = expr.locate_batch_datums(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum &text = expr.args_[0]->locate_expr_datum(ctx, i);
      if (text.is_null()) {
        res_datum[i].set_null();
      } else {
        ObString bin_text = text.get_string();
        char *res_buf = expr.get_str_res_mem(ctx, UuidCommon::LENGTH_UUID, i);
        const char *bin_text_ptr = bin_text.ptr();
        uchar new_bin_text[UuidCommon::BYTE_LENGTH];
        if (OB_ISNULL(res_buf)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(bin_text_ptr) || UuidCommon::BYTE_LENGTH != bin_text.length()) {
          ObString string_type_str("binary");
          ObString func_str(N_BIN_TO_UUID);
          int text_len = std::min(bin_text.length(), UuidCommon::BYTE_LENGTH * 2);
          SMART_VAR(char[UuidCommon::BYTE_LENGTH * 4], hex_buf) {
            int64_t pos = 0;
            if (OB_SUCC(common::hex_print(bin_text.ptr(), text_len, hex_buf, text_len*2, pos))) {
              ret = OB_ERR_INCORRECT_VALUE_FOR_FUNCTION;
              LOG_USER_ERROR(OB_ERR_INCORRECT_VALUE_FOR_FUNCTION,
                            string_type_str.length(), string_type_str.ptr(),
                            text_len*2, hex_buf,
                            func_str.length(), func_str.ptr());
            } else {
            }
          }
        } else  if (need_swap) {
          // The first 4 bytes are restored to "time-low".
          MEMCPY(new_bin_text, bin_text_ptr + 4, 4);
          // Bytes starting with 4th will be restored to "time-mid".
          MEMCPY(new_bin_text + 4, bin_text_ptr + 2, 2);
          // Bytes starting with 6th will be restored to "time-high".
          MEMCPY(new_bin_text + 6, bin_text_ptr, 2);
          // The last 8 bytes were not changed so we just copy them.
          MEMCPY(new_bin_text + 8, bin_text_ptr + 8, 8);
        } else {
          MEMCPY(new_bin_text, bin_text_ptr, UuidCommon::BYTE_LENGTH);
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(UuidCommon::bin2uuid(res_buf, new_bin_text))) {
          } else {
            res_datum[i].set_string(res_buf, UuidCommon::LENGTH_UUID);
          }
        }
      }
      eval_flags.set(i);
    }
  }
  return ret;
}

} //namespace sql
} //namespace oceanbase
