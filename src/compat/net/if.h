#pragma once
#ifdef _WIN32
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif
struct ifreq {
    char ifr_name[IFNAMSIZ];
    struct sockaddr ifr_addr;
};
#else
#include_next <net/if.h>
#endif
