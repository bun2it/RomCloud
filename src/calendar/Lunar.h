#pragma once
// Lịch âm Việt Nam (thuật toán Hồ Ngọc Đức, múi +7).
// solarToLunar() đã kiểm chứng bằng harness với các mốc Tết/Giỗ Tổ/Trung thu.
#include <cmath>

namespace RomCloud {

namespace Lunar {

static const double LUN_PI = 3.1415926535897932385;

inline int jdFromDate(int dd, int mm, int yy) {
    int a = (14 - mm) / 12;
    int y = yy + 4800 - a;
    int m = mm + 12 * a - 3;
    return dd + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

inline double NewMoon(int k) {
    double T = k / 1236.85;
    double T2 = T * T, T3 = T2 * T;
    double dr = LUN_PI / 180.0;
    double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
    Jd1 += 0.00033 * sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
    double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
    double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
    double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
    double C1 = (0.1734 - 0.000393 * T) * sin(M * dr) + 0.0021 * sin(2 * M * dr);
    C1 = C1 - 0.4068 * sin(Mpr * dr) + 0.0161 * sin(2 * Mpr * dr);
    C1 = C1 - 0.0004 * sin(3 * Mpr * dr);
    C1 = C1 + 0.0104 * sin(2 * F * dr) - 0.0051 * sin((M + Mpr) * dr);
    C1 = C1 - 0.0074 * sin((M - Mpr) * dr) + 0.0004 * sin((2 * F + M) * dr);
    C1 = C1 - 0.0004 * sin((2 * F - M) * dr) - 0.0006 * sin((2 * F + Mpr) * dr);
    C1 = C1 + 0.0010 * sin((2 * F - Mpr) * dr) + 0.0005 * sin((2 * Mpr + M) * dr);
    double deltat;
    if (T < -11)
        deltat = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3;
    else
        deltat = -0.000278 + 0.000265 * T + 0.000262 * T2;
    return Jd1 + C1 - deltat;
}

inline int SunLongitude(double jdn) {
    double T = (jdn - 2451545.0) / 36525;
    double T2 = T * T;
    double dr = LUN_PI / 180.0;
    double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
    double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
    double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * sin(dr * M);
    DL += (0.019993 - 0.000101 * T) * sin(dr * 2 * M) + 0.000290 * sin(dr * 3 * M);
    double L = L0 + DL;
    L *= dr;
    L -= LUN_PI * 2 * (int)(L / (LUN_PI * 2));
    return (int)(L / LUN_PI * 6);
}

inline int getNewMoonDay(int k, int timeZone) {
    return (int)floor(NewMoon(k) + 0.5 + timeZone / 24.0);
}

inline int getSunLongitude(int dayNumber, int timeZone) {
    return SunLongitude(dayNumber - 0.5 - timeZone / 24.0);
}

inline int getLunarMonth11(int yy, int timeZone) {
    double off = jdFromDate(31, 12, yy) - 2415021.0;
    int k = (int)(off / 29.530588853);
    int nm = getNewMoonDay(k, timeZone);
    int sunLong = getSunLongitude(nm, timeZone);
    if (sunLong >= 9)
        nm = getNewMoonDay(k - 1, timeZone);
    return nm;
}

inline int getLeapMonthOffset(int a11, int timeZone) {
    int k = (int)((a11 - 2415021.076998695) / 29.530588853 + 0.5);
    int last = 0, i = 1;
    int arc = getSunLongitude(getNewMoonDay(k + i, timeZone), timeZone);
    do {
        last = arc;
        i++;
        arc = getSunLongitude(getNewMoonDay(k + i, timeZone), timeZone);
    } while (arc != last && i < 14);
    return i - 1;
}

struct LunarDate {
    int day = 1, month = 1, year = 2000;
    bool leap = false;
};

inline LunarDate solarToLunar(int dd, int mm, int yy, int timeZone = 7) {
    LunarDate r;
    int dayNumber = jdFromDate(dd, mm, yy);
    int k = (int)((dayNumber - 2415021.076998695) / 29.530588853);
    int moonIndex = k + 1;
    int monthStart = getNewMoonDay(moonIndex, timeZone);
    while (monthStart > dayNumber) {
        moonIndex--;
        monthStart = getNewMoonDay(moonIndex, timeZone);
    }
    while (getNewMoonDay(moonIndex + 1, timeZone) <= dayNumber) {
        moonIndex++;
        monthStart = getNewMoonDay(moonIndex, timeZone);
    }
    int a11 = getLunarMonth11(yy, timeZone);
    int b11 = a11;
    if (a11 >= monthStart) {
        r.year = yy;
        a11 = getLunarMonth11(yy - 1, timeZone);
    } else {
        r.year = yy + 1;
        b11 = getLunarMonth11(yy + 1, timeZone);
    }
    r.day = dayNumber - monthStart + 1;
    // Số tuần trăng từ tháng 11 âm -> số tháng = diff + 11 (wrap 12)
    int diff = (int)((monthStart - a11) / 29);
    r.leap = false;
    r.month = diff + 11;
    // Năm nhuận (dài > 365 ngày) mới có tháng nhuận để lệch số
    if (b11 - a11 > 365) {
        int leapMonthDiff = getLeapMonthOffset(a11, timeZone);
        if (diff >= leapMonthDiff) {
            r.month = diff + 10;
            if (diff == leapMonthDiff)
                r.leap = true;
        }
    }
    if (r.month > 12)
        r.month -= 12;
    // Tháng 11/12 âm đầu năm dương -> thuộc năm âm trước
    if (r.month >= 11 && diff < 4)
        r.year--;
    if (r.month == 0)
        r.month = 12;
    return r;
}

} // namespace Lunar
} // namespace RomCloud
