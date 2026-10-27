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

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

namespace oceanbase
{
namespace sql
{
namespace uuid_node
{

struct NodeDiagnostics
{
  NodeDiagnostics()
      : discovery_operation(nullptr), discovery_errno(0),
        entropy_operation(nullptr), entropy_errno(0), used_random(false)
  {}

  const char *discovery_operation;
  int discovery_errno;
  const char *entropy_operation;
  int entropy_errno;
  bool used_random;
};

inline bool generate_random_node(unsigned char node[6], NodeDiagnostics &diagnostics)
{
  unsigned char random_node[6];
  diagnostics.entropy_operation = nullptr;
  diagnostics.entropy_errno = 0;
  diagnostics.used_random = false;

  int fd;
  do {
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    diagnostics.entropy_errno = errno;
    diagnostics.entropy_operation = "open(/dev/urandom)";
    return false;
  }

  size_t offset = 0;
  while (offset < sizeof(random_node)) {
    const ssize_t count = read(fd, random_node + offset, sizeof(random_node) - offset);
    if (count > 0) {
      offset += static_cast<size_t>(count);
    } else if (count == 0 || errno != EINTR) {
      diagnostics.entropy_errno = count == 0 ? EIO : errno;
      diagnostics.entropy_operation = "read(/dev/urandom)";
      close(fd);
      return false;
    }
  }
  close(fd);

  // RFC 9562 section 6.10: distinguish a random node from a hardware MAC.
  random_node[0] |= 0x01;
  memcpy(node, random_node, sizeof(random_node));
  diagnostics.used_random = true;
  return true;
}

#if defined(__linux__)
inline bool initialize_linux_node(unsigned char node[6], NodeDiagnostics &diagnostics)
{
  diagnostics = NodeDiagnostics();
  unsigned char hardware_node[6];
  bool mac_found = false;
  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    diagnostics.discovery_errno = errno;
    diagnostics.discovery_operation = "socket";
  } else {
    struct ifreq interfaces[1024 / sizeof(struct ifreq)] = {};
    struct ifconf configuration = {};
    configuration.ifc_len = sizeof(interfaces);
    configuration.ifc_buf = reinterpret_cast<char *>(interfaces);
    if (ioctl(sock, SIOCGIFCONF, &configuration) < 0) {
      diagnostics.discovery_errno = errno;
      diagnostics.discovery_operation = "ioctl(SIOCGIFCONF)";
    } else {
      const size_t interface_count = configuration.ifc_len > 0
          ? static_cast<size_t>(configuration.ifc_len) / sizeof(struct ifreq) : 0;
      const size_t interface_capacity = sizeof(interfaces) / sizeof(interfaces[0]);
      for (size_t i = 0; i < interface_count && i < interface_capacity && !mac_found; ++i) {
        struct ifreq request = {};
        memcpy(request.ifr_name, interfaces[i].ifr_name, IFNAMSIZ - 1);
        request.ifr_name[IFNAMSIZ - 1] = '\0';
        if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
          const int error = errno;
          if (diagnostics.discovery_operation == nullptr) {
            diagnostics.discovery_errno = error;
            diagnostics.discovery_operation = "ioctl(SIOCGIFFLAGS)";
          }
        } else if ((request.ifr_flags & IFF_LOOPBACK) == 0) {
          if (ioctl(sock, SIOCGIFHWADDR, &request) < 0) {
            const int error = errno;
            if (diagnostics.discovery_operation == nullptr) {
              diagnostics.discovery_errno = error;
              diagnostics.discovery_operation = "ioctl(SIOCGIFHWADDR)";
            }
          } else if (request.ifr_hwaddr.sa_family == ARPHRD_ETHER) {
            const unsigned char zero_node[6] = {};
            memcpy(hardware_node, request.ifr_hwaddr.sa_data, sizeof(hardware_node));
            mac_found = memcmp(hardware_node, zero_node, sizeof(hardware_node)) != 0;
          }
        }
      }
    }
    close(sock);
  }

  if (mac_found) {
    memcpy(node, hardware_node, sizeof(hardware_node));
    return true;
  }
  if (diagnostics.discovery_operation == nullptr) {
    diagnostics.discovery_operation = "no usable MAC address";
  }
  return generate_random_node(node, diagnostics);
}
#endif

}  // namespace uuid_node
}  // namespace sql
}  // namespace oceanbase
