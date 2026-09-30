#pragma once
#ifdef _WIN32
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
};
#else
#include_next <sys/un.h>
#endif
