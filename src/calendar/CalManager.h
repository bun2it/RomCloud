#pragma once
// ============================================================================
// RomCloud CalManager — lịch phone đẩy qua WebServer, Brick lưu lại.
// Nguồn: link ICS đăng ký (iCloud/Google "secret address") tự refresh,
// paste text .ics, thêm tay. Parse VEVENT + RRULE cơ bản (DAILY/WEEKLY/
// MONTHLY/YEARLY + COUNT/UNTIL). Lưu SQLite (cal_sources, cal_events).
// ============================================================================
#include <string>
#include <vector>
#include <cstdint>

namespace RomCloud {

struct CalEvent {
    int64_t id = 0;
    std::string source; // tên nguồn hoặc "manual"
    std::string uid;
    std::string title;
    int64_t startTs = 0; // epoch local
    int64_t endTs = 0;
    bool allDay = false;
    bool remind = false; // true = tới giờ thì kêu báo thức
};

struct CalSource {
    int64_t id = 0;
    std::string name;
    std::string url;
    int64_t lastRefresh = 0;
};

class CalManager {
public:
    static CalManager& instance();

    void ensureTables();
    // Trả về số event import được, <0 nếu lỗi
    int importIcsText(const std::string& ics, const std::string& sourceName);
    int64_t addUrlSource(const std::string& name, const std::string& url);
    bool deleteSource(int64_t id);
    std::vector<CalSource> sources();
    int64_t addManual(const std::string& dateYmd, const std::string& timeHm,
                      const std::string& title); // date 2026-10-05, time 08:30
    bool deleteEvent(int64_t id);
    bool setRemind(int64_t id, bool on); // chọn báo / không báo từng sự kiện
    std::vector<CalEvent> eventsForRange(int64_t fromTs, int64_t toTs);
    std::vector<CalEvent> upcoming(int limit = 10);
    // Refresh các URL quá 24h (gọi khi mở trang Lịch, chạy nền)
    int refreshStale();
    // Refresh TẤT CẢ nguồn (nút Refresh trên web) — bỏ qua cổng 24h
    int refreshAll();

private:
    CalManager() = default;
    struct RawEvent {
        std::string uid, title;
        int64_t startTs = 0, endTs = 0;
        bool allDay = false;
        std::string rrule;
    };
    static std::string unfold(const std::string& ics);
    static std::string prop(const std::string& vevent, const std::string& key);
    static std::string propParam(const std::string& vevent, const std::string& key,
                                 const std::string& param);
    static int64_t parseDt(const std::string& val, bool& allDay,
                           const std::string& tzid = "");
    static int64_t mktimeTz(struct tm& t, const std::string& tzid);
    static std::vector<RawEvent> expand(const RawEvent& e);
    void replaceSourceEvents(int64_t sourceId, const std::string& sourceName,
                             const std::vector<RawEvent>& events);
};

} // namespace RomCloud
