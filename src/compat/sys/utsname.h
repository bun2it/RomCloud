#pragma once
#ifdef _WIN32
#include <string.h>

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

inline int uname(struct utsname* u) {
    if (u) {
        strcpy(u->sysname, "Windows");
        strcpy(u->nodename, "PC-Simulator");
        strcpy(u->release, "10.0");
        strcpy(u->version, "PC");
        strcpy(u->machine, "x86_64");
    }
    return 0;
}
#else
#include_next <sys/utsname.h>
#endif
