#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <direct.h>
#include <io.h>
#include <stdlib.h>

inline int portable_mkdir(const char* path, int mode = 0755) {
    (void)mode;
    return _mkdir(path);
}
#define mkdir(p, ...) portable_mkdir(p, ##__VA_ARGS__)

inline void sync() {}

#ifndef lstat
#define lstat stat
#endif

#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif

inline int rand_r(unsigned int*) {
    return rand();
}

inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name) != nullptr) return 0;
    return _putenv_s(name, value);
}

#endif
