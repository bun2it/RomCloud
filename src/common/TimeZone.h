#pragma once
// TimeZone: đổi múi giờ KHÔNG race. Mọi setenv(TZ) toàn cục đều qua 1 mutex
// duy nhất ở đây (CalManager + ClockUI từng tự setenv riêng -> giẫm nhau
// giữa các thread làm lệch ngày/giờ hiển thị).
#include <ctime>
#include <string>
#include <mutex>
#include <cstdlib>

namespace RomCloud {

class TimeZone {
public:
    // UTC offset (giây, đông = +) của tzid tại thời điểm t.
    // tzid rỗng = múi giờ máy (không chạm env).
    static long offsetFor(const std::string& tzid, std::time_t t) {
        if (tzid.empty()) {
            struct tm g;
            gmtime_r(&t, &g);
            struct tm gc = g;
            return (long)(t - mktime(&gc));
        }
        std::lock_guard<std::mutex> lk(mutex());
        const char* old = getenv("TZ");
        std::string oldTz = old ? old : "";
        bool hadOld = old != nullptr;
        setenv("TZ", tzid.c_str(), 1);
        tzset();
        struct tm g;
        gmtime_r(&t, &g);
        struct tm gc = g;
        long off = (long)(t - mktime(&gc));
        if (hadOld) setenv("TZ", oldTz.c_str(), 1);
        else unsetenv("TZ");
        tzset();
        return off;
    }

    // Giờ wall-clock của tzid tại t (không đụng env ngoài offsetFor).
    static bool wallClock(const std::string& tzid, std::time_t t, struct tm& out) {
        long off = offsetFor(tzid.empty() ? "" : tzid, t);
        if (tzid.empty()) {
            localtime_r(&t, &out);
            return true;
        }
        std::time_t shifted = t + off;
        gmtime_r(&shifted, &out);
        return true;
    }

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
};

} // namespace RomCloud
