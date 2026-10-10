// compat_shim.c — LD_PRELOAD lấp symbol còn thiếu trên glibc 2.33 của máy.
//
// Electron/Chromium 108 (và NSS đi kèm) reference một số symbol mà
// glibc 2.33 của TrimUI không có:
//   - localtime64/localtime64_r: có từ glibc mới; trên aarch64 time_t đã
//     64-bit nên forward thẳng sang localtime/localtime_r là đúng tuyệt đối.
//   - nspr_use_zone_allocator: cờ data của NSPR; 0 = hành vi mặc định.
//
// Build: zig cc -target aarch64-linux-gnu.2.33 -O2 -shared -fPIC
//        src/browser/chrome/compat_shim.c -o build/rc-compat_shim.so -ldl
// Dùng: LD_PRELOAD=<app>/rc-web/compat_shim.so electron ...
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

struct tm;

static struct tm *(*real_localtime)(const void *) = 0;
static struct tm *(*real_localtime_r)(const void *, struct tm *) = 0;

struct tm *localtime64(const long long *t) {
    if (real_localtime == 0)
        real_localtime =
            (struct tm *(*)(const void *))dlsym(RTLD_NEXT, "localtime");
    if (real_localtime == 0) return 0;
    long long v = (t != 0) ? *t : 0;
    return real_localtime(&v);
}

struct tm *localtime64_r(const long long *t, struct tm *res) {
    if (real_localtime_r == 0)
        real_localtime_r =
            (struct tm *(*)(const void *, struct tm *))dlsym(RTLD_NEXT,
                                                             "localtime_r");
    if (real_localtime_r == 0) return 0;
    long long v = (t != 0) ? *t : 0;
    return real_localtime_r(&v, res);
}

/* Cờ NSPR: 0 = không dùng zone allocator (mặc định an toàn). */
int nspr_use_zone_allocator = 0;
