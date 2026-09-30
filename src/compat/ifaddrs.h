#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
struct ifaddrs {
    struct ifaddrs* ifa_next;
    char* ifa_name;
    unsigned int ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
};
inline int getifaddrs(struct ifaddrs** ifap) { if (ifap) *ifap = nullptr; return 0; }
inline void freeifaddrs(struct ifaddrs*) {}
#else
#include_next <ifaddrs.h>
#endif
