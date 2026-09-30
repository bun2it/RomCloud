#pragma once
#ifdef _WIN32
struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    unsigned long f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};
inline int statvfs(const char*, struct statvfs* buf) {
    if (buf) {
        buf->f_frsize = 4096;
        buf->f_blocks = 16 * 1024 * 1024; // ~64GB
        buf->f_bfree = 8 * 1024 * 1024;
        buf->f_bavail = 8 * 1024 * 1024;
    }
    return 0;
}
#else
#include_next <sys/statvfs.h>
#endif
