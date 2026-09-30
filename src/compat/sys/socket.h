#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif

template <typename T>
inline int portable_setsockopt(SOCKET s, int level, int optname, const T* optval, int optlen) {
    return ::setsockopt(s, level, optname, reinterpret_cast<const char*>(optval), optlen);
}
#define setsockopt(s, lvl, opt, val, len) portable_setsockopt(s, lvl, opt, val, static_cast<int>(len))

#else
#include_next <sys/socket.h>
#endif
