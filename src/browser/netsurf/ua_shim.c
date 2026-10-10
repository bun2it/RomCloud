// netsurf_ua_shim.c — LD_PRELOAD shim cho NetSurf daemon (FULL Daemon mode).
//
// NetSurf 3.11 dựng User-Agent cứng: "Mozilla/5.0 (<sysname>) NetSurf/x.y"
// và ép desktop ("X11; Linux") khi sysname == "Linux"
// (libnetsurf-src/netsurf/utils/useragent.c). Không có option nào đổi được.
//
// Shim này override uname(): giữ nguyên mọi field, chỉ thay sysname thành
// chuỗi mobile. Kết quả: "Mozilla/5.0 (Linux; Android 13; Mobile) NetSurf/3.11"
// -> các site trả bản mobile HTML/CSS, đúng "chuẩn mobile" cho Brick.
//
// Build: zig cc -target aarch64-linux-gnu.2.33 -O2 -shared -fPIC
//        src/browser/netsurf/ua_shim.c -o lib/netsurf_ua_shim.so
// Dùng: LD_PRELOAD=<app>/lib/netsurf_ua_shim.so bin/netsurf ...
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <sys/utsname.h>

typedef int (*uname_fn)(struct utsname *buf);

int uname(struct utsname *buf) {
    static uname_fn real_uname = 0;
    if (real_uname == 0) {
        real_uname = (uname_fn)dlsym(RTLD_NEXT, "uname");
        if (real_uname == 0) return -1;
    }
    int r = real_uname(buf);
    if (r == 0 && buf != 0) {
        static const char mobile[] = "Linux; Android 13; Mobile";
        size_t n = sizeof(buf->sysname);
        strncpy(buf->sysname, mobile, n - 1);
        buf->sysname[n - 1] = '\0';
    }
    return r;
}
