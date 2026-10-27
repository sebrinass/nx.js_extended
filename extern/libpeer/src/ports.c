#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

#if CONFIG_USE_LWIP
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#elif defined(__SWITCH__)
#include <unistd.h>
#include <netdb.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/ioctl.h>
#endif

#include "ports.h"
#include "utils.h"

#if defined(__SWITCH__)
// The Switch has no getifaddrs(): the compatibility stub returns an empty
// list, so a host candidate could never be built and only the public STUN
// candidate survived -> no direct LAN path.
//
// The engine owns the Switch specific networking, so it hands the local IPv4
// address in through a weak hook (engine source/nx_webrtc_net.cc).  The hook
// is weak so libpeer still links on its own; the gethostid() fallback below
// is used when the engine is not linked in.
#include <stdint.h>

#if defined(__GNUC__)
extern int nx_libpeer_get_local_ipv4(uint32_t* out) __attribute__((weak));
#endif

// The address is handed over as a network order "struct in_addr", i.e. the
// four bytes in memory are the four address octets.  Test the octets and not
// the integer value so the check does not depend on the host endianness.
static int switch_ipv4_is_usable(uint32_t ip) {
  uint8_t octets[4];
  memcpy(octets, &ip, sizeof(octets));
  if (octets[0] == 0) {
    return 0;  // 0.0.0.0 - no address yet
  }
  if (octets[0] == 127) {
    // 127.0.0.0/8 - gethostid() falls back to INADDR_LOOPBACK when its nifm
    // query fails, and a loopback ICE candidate is worse than no candidate.
    return 0;
  }
  if (octets[0] == 169 && octets[1] == 254) {
    return 0;  // 169.254.0.0/16 - link local, not routable
  }
  return 1;
}
#endif

int ports_get_host_addr(Address* addr, const char* iface_prefix) {
  int ret = 0;

#if CONFIG_USE_LWIP
  struct netif* netif;
  int i;
  for (netif = netif_list; netif != NULL; netif = netif->next) {
    switch (addr->family) {
      case AF_INET6:
        for (i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
          if (!ip6_addr_isany(netif_ip6_addr(netif, i))) {
            memcpy(&addr->sin6.sin6_addr, netif_ip6_addr(netif, i), 16);
            ret = 1;
            break;
          }
        }
        break;
      case AF_INET:
      default:
        if (!ip_addr_isany(&netif->ip_addr)) {
          memcpy(&addr->sin.sin_addr, &netif->ip_addr.u_addr.ip4, 4);
          ret = 1;
        }
        break;
    }

    if (ret) {
      break;
    }
  }
#elif defined(__SWITCH__)
  uint32_t ip = 0;
  int have_ip = 0;
#if defined(__GNUC__)
  if (nx_libpeer_get_local_ipv4) {
    have_ip = nx_libpeer_get_local_ipv4(&ip) ? 1 : 0;
  }
#endif
  if (!have_ip) {
    // Fallback for a libpeer that is not linked against the engine.
    ip = (uint32_t)gethostid();
    have_ip = 1;
  }
  if (have_ip && addr->family == AF_INET && switch_ipv4_is_usable(ip)) {
      memcpy(&addr->sin.sin_addr, &ip, 4);
      ret = 1;
  }
#else

  struct ifaddrs *ifaddr, *ifa;

  if (getifaddrs(&ifaddr) == -1) {
    LOGE("getifaddrs failed: %s", strerror(errno));
    return -1;
  }

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL) {
      continue;
    }

    if (ifa->ifa_addr->sa_family != addr->family) {
      continue;
    }

    if (iface_prefix && strlen(iface_prefix) > 0) {
      if (strncmp(ifa->ifa_name, iface_prefix, strlen(iface_prefix)) != 0) {
        continue;
      }

    } else {
      if ((ifa->ifa_flags & IFF_UP) == 0) {
        continue;
      }

      if ((ifa->ifa_flags & IFF_RUNNING) == 0) {
        continue;
      }

      if ((ifa->ifa_flags & IFF_LOOPBACK) == IFF_LOOPBACK) {
        continue;
      }
    }

    switch (ifa->ifa_addr->sa_family) {
      case AF_INET6:
        memcpy(&addr->sin6, ifa->ifa_addr, sizeof(struct sockaddr_in6));
        break;
      case AF_INET:
      default:
        memcpy(&addr->sin, ifa->ifa_addr, sizeof(struct sockaddr_in));
        break;
    }
    ret = 1;
    break;
  }
  freeifaddrs(ifaddr);
#endif
  return ret;
}

int ports_resolve_addr(const char* host, Address* addr) {
  char addr_string[ADDRSTRLEN];
  int ret = -1;
  struct addrinfo hints, *res, *p;
  int status;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if ((status = getaddrinfo(host, NULL, &hints, &res)) != 0) {
    LOGE("getaddrinfo error: %d\n", status);
    return ret;
  }

  // TODO: Support for IPv6
  addr_set_family(addr, AF_INET);
  for (p = res; p != NULL; p = p->ai_next) {
    if (p->ai_family == addr->family) {
      switch (addr->family) {
        case AF_INET6:
          memcpy(&addr->sin6, p->ai_addr, sizeof(struct sockaddr_in6));
          break;
        case AF_INET:
        default:
          memcpy(&addr->sin, p->ai_addr, sizeof(struct sockaddr_in));
          break;
      }
      ret = 0;
    }
  }

  addr_to_string(addr, addr_string, sizeof(addr_string));
  LOGI("Resolved %s -> %s", host, addr_string);
  freeaddrinfo(res);
  return ret;
}

uint32_t ports_get_epoch_time() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

uint32_t ports_get_monotonic_time(void) {
#if CONFIG_USE_LWIP
  return sys_now();
#else
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
    return 0;
  return (uint32_t)time.tv_sec * 1000 + (uint32_t)(time.tv_nsec / 1000000);
#endif
}

uint64_t ports_get_monotonic_time_us(void) {
#if CONFIG_USE_LWIP
  return (uint64_t)sys_now() * 1000;
#else
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
    return 0;
  return (uint64_t)time.tv_sec * 1000000 + (uint64_t)(time.tv_nsec / 1000);
#endif
}

void ports_sleep_ms(int ms) {
#if CONFIG_USE_LWIP
  sys_msleep(ms);
#else
  usleep(ms * 1000);
#endif
}
