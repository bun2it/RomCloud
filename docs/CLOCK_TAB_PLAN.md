# Kế hoạch tab ĐỒNG HỒ (giờ thế giới + báo thức briefing + Pomodoro)

> Trạng thái: đang làm. Tabs trang Thời tiết: THỜI TIẾT | ĐỒNG HỒ | CAMERA.

## 1. Phạm vi (lấy lõi từ repo tham khảo, bỏ map/pet/scrub)
- World clock cards: Hà Nội (home) + Tokyo + London + New York — giờ,
  lệch so với nhà, ngày. Đổi múi qua zoneinfo máy (`TZ` trick, đã verify
  có `Asia/Ho_Chi_Minh`; zone thiếu thì fallback UTC, không crash).
- Báo thức: nhiều giờ, bật/tắt từng cái, lưu DB (`clock_alarms`).
  Kêu khi app đang chạy (máy sleep thì KHÔNG kêu được — ghi rõ trong UI).
- Morning briefing khi chuông kêu: thời tiết hôm nay (cache) + việc đầu
  tiên trong ngày (lịch phone) + câu gợi ý (mưa → xuất phát sớm 15 phút).
- Pomodoro: 25 làm / 5 nghỉ, A bắt đầu/tạm dừng, X đặt lại, hết phase
  kêu + toast. Session-only.
- Tiếng kêu: beep sine SDL audio (880Hz, 3 hồi), không cần file âm thanh.

## 2. Kỹ thuật
- `src/clock/ClockStore.{h,cpp}`: CRUD alarms (settings key), beep start/stop.
- Modal báo thức: `UIState::CLOCK_ALARM` (A tắt, B hoãn 10 phút).
- Poll trong `UIManager::update()` mỗi 5s: khớp HH:MM + chưa kêu hôm nay
  → mở modal. Pomodoro tick cùng chỗ.
- Tab pills 3 mục; L1/R1 xoay vòng `(m_wxTab+1)%3` (camera dời sang 2).
- Không đụng luồng phát video/search/OTA.

## 3. Test (máy thật)
- Đặt báo thức +1 phút → đúng giờ hiện modal + beep + briefing đúng
  thời tiết/lịch hôm đó. B hoãn → kêu lại sau 10 phút.
- Pomodoro chạy hết 25:00 → kêu + chuyển nghỉ 5:00.
- Đổi múi giờ máy / mất mạng: giờ các thành phố vẫn đúng, briefing
  dùng cache + ghi rõ dữ liệu cũ.
