#pragma once

#include <string>

namespace RomCloud {
namespace UiStrings {

// ============================================================================
// CẤU HÌNH GIAO DIỆN — ROMCLOUD CHO TRIMUI BRICK PRO
// Chỉnh sửa chuỗi văn bản tại đây và biên dịch lại.
// ============================================================================

// --- 1. TIÊU ĐỀ CHUNG & HEADER ---
inline const char *APP_TITLE = "ROMCLOUD";
inline const char *APP_SUBTITLE = "TRIMUI BRICK PRO";
inline const char *SYSTEM_SELECT_TITLE = "CHỌN HỆ MÁY";
inline const char *HEADER_SYSTEM_SELECT = "CHỌN HỆ MÁY";
inline const char *HEADER_SEARCH = "TÌM KIẾM";
inline const char *HEADER_SETTINGS = "CÀI ĐẶT";
inline const char *HEADER_WEB_CONNECT = "KẾT NỐI DRIVE";
inline const char *HEADER_SYNC = "ĐỒNG BỘ";
inline const char *HEADER_DOWNLOAD = "ĐANG TẢI";
inline const char *HEADER_DIAG = "THÔNG TIN";
inline const char *HEADER_OTA = "CẬP NHẬT";

// --- 2. MENU CHÍNH (MAIN MENU) ---
inline const char *MENU_PLAY = "THƯ VIỆN GAME";
inline const char *MENU_SYNC = "ĐỒNG BỘ";
inline const char *MENU_REVERSE_SYNC = "TẢI LÊN DRIVE";
inline const char *MENU_OTA = "CẬP NHẬT";
inline const char *MENU_OTA_NEW_BADGE = "🆕 OTA v";
inline const char *MENU_SETTINGS = "CÀI ĐẶT";
inline const char *MENU_DIAG = "THÔNG TIN";
inline const char *MENU_EXIT = "THOÁT";

// --- 3. THANH ĐIỀU KHIỂN & NÚT BẤM ---
inline const char *FOOTER_ENTER_SYSTEM = "VÀO";
inline const char *FOOTER_MAIN_MENU = "MENU";
inline const char *FOOTER_SYNC_DRIVE = "ĐỒNG BỘ";
inline const char *FOOTER_CHANGE_PAGE = "TRANG";
inline const char *FOOTER_ADD_QUEUE = "+HÀNG";
inline const char *FOOTER_BACK = "QUAY LẠI";
inline const char *FOOTER_CANCEL = "HỦY";
inline const char *FOOTER_DELETE = "XÓA";
inline const char *FOOTER_SYNC = "ĐỒNG BỘ";
inline const char *FOOTER_FILTER = "LỌC";
inline const char *FOOTER_CONFIRM = "XÁC NHẬN";
inline const char *FOOTER_SELECT = "CHỌN";

inline const char *BTN_ENTER_SYSTEM = "VÀO HỆ MÁY";
inline const char *BTN_MAIN_MENU = "MENU CHÍNH";
inline const char *BTN_SYNC_DRIVE = "ĐỒNG BỘ";
inline const char *BTN_CHANGE_PAGE = "CHUYỂN TRANG";
inline const char *BTN_ADD_QUEUE = "+ HÀNG TẢI";
inline const char *BTN_BACK = "QUAY LẠI";
inline const char *BTN_CANCEL = "HỦY";
inline const char *BTN_DELETE_QUEUE = "BỎ HÀNG";
inline const char *BTN_DELETE_ROM_SD = "XÓA KHỎI THẺ";
inline const char *BTN_DELETE_ROM = "XÓA ROM";
inline const char *BTN_CONFIRM_DELETE_SD = "XÁC NHẬN XÓA";
inline const char *BTN_CONFIRM_DELETE = "[A] Xóa";
inline const char *BTN_CANCEL_DELETE = "[B] Hủy";
inline const char *BTN_SYNC = "ĐỒNG BỘ";
inline const char *BTN_FILTER = "BỘ LỌC";
inline const char *BTN_CONFIRM = "XÁC NHẬN";
inline const char *BTN_SELECT = "CHỌN";
inline const char *BTN_JUMP_ALPHA = "NHẢY CHỮ";
inline const char *BTN_SEARCH = "TÌM KIẾM";
inline const char *BTN_SELECT_CHAR = "CHỌN";
inline const char *BTN_CLEAR_ALL = "XÓA HẾT";
inline const char *BTN_DOWNLOAD = "TẢI VỀ";
inline const char *BTN_CANCEL_ACTION = "HỦY";
inline const char *BTN_BACK_MAIN_MENU_HINT = "[B] Menu";

// --- 4. MÀN HÌNH CHỌN HỆ MÁY ---
inline const char *SYS_SELECT_TITLE = "CHỌN HỆ MÁY";
inline const char *SYS_SELECT_SYSTEMS_LABEL = "Hệ máy | ";
inline const char *SYS_SELECT_GAMES_LOCAL = "Thẻ | ";
inline const char *SYS_SELECT_GAMES_CLOUD = "Cloud)";
inline const char *SYS_SELECT_SDCARD_PREFIX = "THẺ: ";
inline const char *SYS_SELECT_FOLDER_PREFIX = "/Roms/";
inline const char *SYS_SELECT_EXT_PREFIX = " • ";

// --- 5. MÀN HÌNH TÌM KIẾM ---
inline const char *SEARCH_PROMPT_MIN_CHARS = "Nhập ít nhất 2 ký tự...";
inline const char *SEARCH_NO_RESULTS = "Không tìm thấy.";
inline const char *SEARCH_RESULTS_SUFFIX = " kết quả";
inline const char *SEARCH_NAV_UP_HINT = "◀ Lên bàn phím";
inline const char *SEARCH_NAV_DOWN_HINT = "▼ Xem kết quả";

// --- 6. DANH SÁCH GAME & CHI TIẾT GAME ---
inline const char *GAME_LIST_EMPTY = "Không có game.";
inline const char *GAME_FILTER_HINT = "[SELECT] Đổi bộ lọc";
inline const char *FILTER_TAG_ALL = "TẤT CẢ";
inline const char *FILTER_TAG_LOCAL = "THẺ NHỚ";
inline const char *FILTER_TAG_CLOUD = "CLOUD";
inline const char *FILTER_LABEL_ALL = "Lọc: TẤT CẢ";
inline const char *FILTER_LABEL_LOCAL = "Lọc: THẺ NHỚ";
inline const char *FILTER_LABEL_CLOUD = "Lọc: CLOUD";
inline const char *BADGE_LOCAL = "THẺ";
inline const char *BADGE_CLOUD = "CLOUD";
inline const char *BADGE_DOWNLOADED = "✓ ĐÃ TẢI";
inline const char *BADGE_DOWNLOADING = "ĐANG TẢI...";
inline const char *BADGE_DOWNLOADING_PCT = "Tải ";
inline const char *BADGE_QUEUED = "CHỜ";
inline const char *BADGE_DELETE_BTN = "[X] XÓA";
inline const char *BADGE_CANCEL_DL_BTN = "[X] HỦY";
inline const char *BADGE_REMOVE_QUEUE_BTN = "[X] BỎ";
inline const char *BADGE_ADD_QUEUE_BTN = "[A] TẢI";
inline const char *DETAIL_SYSTEM = "Hệ máy:";
inline const char *DETAIL_LOCATION = "Vị trí:";
inline const char *DETAIL_SIZE = "Dung lượng:";
inline const char *DETAIL_FILENAME = "Tên file:";
inline const char *DETAIL_SYS_LABEL = "Hệ máy:";
inline const char *DETAIL_SIZE_LABEL = "Dung lượng:";
inline const char *DETAIL_LOCATION_LABEL = "Lưu tại:";
inline const char *DETAIL_SD_PATH_PREFIX = "Thẻ (/Roms/";
inline const char *DETAIL_LOCAL_STORAGE = "Thẻ nhớ MicroSD";
inline const char *DETAIL_CLOUD_STORAGE = "Google Drive";
inline const char *QUEUE_DOWNLOADING_ACTIVE = "Đang tải, còn ";
inline const char *QUEUE_REMAINING_SUFFIX = " game chờ.";
inline const char *QUEUE_DOWNLOADING_EMPTY = "Hàng trống.";
inline const char *QUEUE_WAITING_PREFIX = "Hàng: ";

// --- 7. HỘP THOẠI XÓA ROM ---
inline const char *DELETE_TITLE = "XÓA ROM?";
inline const char *DELETE_DESC = "Xóa file khỏi thẻ SD.";
inline const char *DELETE_CLOUD_SAFE = "Bản trên Drive vẫn an toàn.";
inline const char *DELETE_CONFIRM_BTN = "[A] Xóa";
inline const char *DELETE_CANCEL_BTN = "[B] Hủy";
inline const char *DIALOG_DELETE_TITLE = "XÁC NHẬN XÓA";
inline const char *DIALOG_DELETE_FILE_LABEL = "File: ";
inline const char *DIALOG_DELETE_PROMPT = "Xóa ROM khỏi thẻ nhớ?";
inline const char *DIALOG_DELETE_SAFE_HINT = "Bản Drive vẫn an toàn.";

// --- 8. TUYÊN BỐ BẢN QUYỀN ---
inline const char *DISCLAIMER_TITLE = "XÁC NHẬN";
inline const char *DISCLAIMER_SUBTITLE = "Kiểm tra quyền sở hữu trước khi kết nối";
inline const char *DISCLAIMER_SEC1_TITLE = "Phạm vi:";
inline const char *DISCLAIMER_SEC1_LINE1 = "RomCloud chỉ đồng bộ dữ liệu cá nhân.";
inline const char *DISCLAIMER_SEC1_LINE2 = "Không lưu trữ hay phân phối ROM.";
inline const char *DISCLAIMER_SEC2_TITLE = "Nguồn dữ liệu:";
inline const char *DISCLAIMER_SEC2_LINE1 = "ROM từ Google Drive của bạn.";
inline const char *DISCLAIMER_SEC2_LINE2 = "Chỉ nội dung bạn có quyền truy cập.";
inline const char *DISCLAIMER_SEC3_TITLE = "Trách nhiệm:";
inline const char *DISCLAIMER_SEC3_LINE1 = "Bạn chịu trách nhiệm về bản quyền";
inline const char *DISCLAIMER_SEC3_LINE2 = "ROM trên tài khoản Drive của mình.";
inline const char *DISCLAIMER_SEC3_LINE3 = "";
inline const char *DISCLAIMER_AGREE = "[A] Đồng ý";
inline const char *DISCLAIMER_DECLINE = "[B] Từ chối";

// --- 9. CÀI ĐẶT HỆ THỐNG ---
inline const char *SETTING_DRIVE_STATUS = "Google Drive:";
inline const char *SETTING_CONNECTED = "ĐÃ KẾT NỐI";
inline const char *SETTING_DISCONNECTED = "CHƯA KẾT NỐI";
inline const char *SETTING_LOGOUT_BTN = "[X] Đăng xuất";
inline const char *SETTING_CONNECT_WEB_BTN = "[A] Kết nối";
inline const char *SETTING_DRIVE_FOLDER = "Thư mục Drive:";
inline const char *SETTING_NOT_CONFIGURED = "Chưa thiết lập";
inline const char *SETTING_ROM_SD_FOLDER = "ROM trên thẻ:";
inline const char *SETTING_LAST_SYNC = "Đồng bộ cuối:";
inline const char *SETTING_NEVER_SYNCED = "Chưa đồng bộ";
inline const char *SETTING_SQLITE_DB = "Cơ sở dữ liệu:";
inline const char *SETTING_SCAN_MODE = "Quét Drive:";
inline const char *SETTING_SCAN_AUTO = "Tự động";
inline const char *SETTING_WEB_PORTAL = "Portal:";
inline const char *SETTING_COVER_CACHE = "Bộ nhớ đệm ảnh:";
inline const char *SETTING_COVER_CACHE_VAL = "SDL2_image (tối đa 64 ảnh)";

// --- 10. KẾT NỐI QUA WEB ---
inline const char *WEB_CONNECT_TITLE = "KẾT NỐI DRIVE";
inline const char *WEB_CONNECT_GUIDE_TITLE = "KẾT NỐI TÀI KHOẢN";
inline const char *WEB_CONNECT_STEP1 = "BƯỚC 1: Kết nối điện thoại/PC cùng Wi-Fi.";
inline const char *WEB_CONNECT_STEP2 = "BƯỚC 2: Mở trình duyệt truy cập:";
inline const char *WEB_CONNECT_STEP3 = "BƯỚC 3: Đăng nhập Google hoặc nhập liên kết Drive.";
inline const char *WEB_CONNECT_WAITING = "* Đang chờ kết nối...";
inline const char *WEB_CONNECT_AUTO_HINT = "Tự động hoàn tất khi đăng nhập.";
inline const char *WEB_CONNECT_BACK_BTN = "[B] Quay lại";
inline const char *WEB_CONNECT_SUCCESS = "KẾT NỐI THÀNH CÔNG!";
inline const char *WEB_CONNECT_ACCOUNT_PREFIX = "Tài khoản:";
inline const char *WEB_CONNECT_READY = "Kho game sẵn sàng tải về.";
inline const char *WEB_CONNECT_START_BTN = "[A] Bắt đầu";

// --- 11. ĐỒNG BỘ GOOGLE DRIVE ---
inline const char *SYNC_TITLE = "ĐỒNG BỘ";
inline const char *SYNC_CONNECTING = "Đang kết nối...";
inline const char *SYNC_SCANNING = "Đang quét game trên Drive...";
inline const char *SYNC_CANCEL_HINT = "[B] Hủy";
inline const char *SYNC_API_CONNECTING = "Đang kết nối API...";
inline const char *SYNC_AUTH_TOKEN = "Đang xác thực...";
inline const char *SYNC_SCANNING_GAMES = "Đang quét game...";
inline const char *SYNC_SEARCH_FOLDERS = "Tìm thư mục RomCloud...";
inline const char *SYNC_SCANNING_SYS_PREFIX = "Quét: ";
inline const char *SYNC_SYS_PREFIX = "Hệ máy ";
inline const char *SYNC_SAVED_PREFIX = "Đã lưu: ";
inline const char *SYNC_NEW_PREFIX = " (Mới: ";
inline const char *SYNC_UPDATED_PREFIX = ", Cập nhật: ";
inline const char *SYNC_CANCEL_BTN = "[B] Hủy";

// --- 11B. REVERSE SYNC (UPLOAD) ---
inline const char *REVERSE_SYNC_TITLE = "TẢI LÊN DRIVE";
inline const char *REVERSE_SYNC_PREPARING = "Đang quét game trên thẻ...";
inline const char *REVERSE_SYNC_UPLOADING = "Đang tải lên...";
inline const char *REVERSE_SYNC_GAME_PROGRESS = "Đang tải: ";
inline const char *REVERSE_SYNC_STATS = "Tiến độ: ";
inline const char *REVERSE_SYNC_SUCCESS = "Hoàn tất!";
inline const char *REVERSE_SYNC_SUCCESS_SUF = " game đã tải lên.";
inline const char *REVERSE_SYNC_FAILED = "Thất bại: ";
inline const char *REVERSE_SYNC_CANCELLED = "Đã hủy.";
inline const char *REVERSE_SYNC_NO_GAMES = "Không có game cần tải lên.";
inline const char *REVERSE_SYNC_NOT_LINKED = "Chưa kết nối Drive. Vào Cài đặt.";
inline const char *REVERSE_SYNC_BTN = "[Y] Tải lên";
inline const char *REVERSE_SYNC_CANCEL_BTN = "[B] Hủy";
inline const char *REVERSE_SYNC_GAMES_FOUND = "Tìm thấy ";
inline const char *REVERSE_SYNC_GAMES_SUF = " game chưa có trên Cloud.";

// --- 12. TIẾN TRÌNH TẢI ROM ---
inline const char *DL_SYS_PREFIX = "Hệ máy: ";
inline const char *DL_FILE_PREFIX = " • File: ";
inline const char *DL_CONNECTING = "Đang kết nối...";
inline const char *DL_CHECKING_FILE = "Kiểm tra file...";
inline const char *DL_FROM_DRIVE = "Đang tải từ Drive...";
inline const char *DL_REMAINING_QUEUE_PREFIX = "Còn ";
inline const char *DL_REMAINING_QUEUE_SUFFIX = " game trong hàng.";
inline const char *DL_CANCEL_BTN = "[B] Hủy";

// --- 13. THÔNG TIN HỆ THỐNG ---
inline const char *DIAG_HW_DEVICE = "Thiết bị";
inline const char *DIAG_CPU_ARCH = "CPU";
inline const char *DIAG_OS_KERNEL = "Hệ điều hành";
inline const char *DIAG_RAM = "Bộ nhớ RAM";
inline const char *DIAG_FREE_PREFIX = "Còn trống ";
inline const char *DIAG_TOTAL_SEPARATOR = " / ";
inline const char *DIAG_DISPLAY = "Màn hình";
inline const char *DIAG_SDL2_GFX = "Đồ họa SDL2";
inline const char *DIAG_HW_ACCEL = " (Tăng tốc)";
inline const char *DIAG_SQLITE_DB = "Cơ sở dữ liệu";
inline const char *DIAG_SCHEMA_VER_PREFIX = " (Cấu trúc v";
inline const char *DIAG_SD_STORAGE = "Dung lượng thẻ";
inline const char *DIAG_GAMEPAD = "Phím điều khiển";
inline const char *DIAG_WIFI = "Wi-Fi";
inline const char *DIAG_SAFETY = "Độ an toàn";
inline const char *DIAG_SAFETY_VAL = "100% — Không sửa hệ thống";
inline const char *DIAG_VAL_64BIT = " (64-bit)";
inline const char *DIAG_VAL_HW_ACCEL = " (Tăng tốc)";
inline const char *DIAG_VAL_SCHEMA_PREFIX = " • Cấu trúc v";

// --- 14. CẬP NHẬT OTA ---
inline const char *OTA_TITLE = "CẬP NHẬT";
inline const char *OTA_CHECKING = "Đang kiểm tra...";
inline const char *OTA_DEV_CURRENT_VER = "Phiên bản hiện tại: v";
inline const char *OTA_DEV_SOURCE_PREFIX = "Nguồn: github.com/";
inline const char *OTA_STATUS_IDLE = "Nhấn [A] để kiểm tra cập nhật.";
inline const char *OTA_STATUS_CHECKING = "Đang kiểm tra...";
inline const char *OTA_STATUS_UPDATE_AVAILABLE = "CÓ BẢN MỚI!";
inline const char *OTA_STATUS_UP_TO_DATE = "ĐÃ LÀ BẢN MỚI NHẤT";
inline const char *OTA_STATUS_FAILED = "KIỂM TRA THẤT BẠI";
inline const char *OTA_STATUS_DOWNLOADING = "ĐANG TẢI...";
inline const char *OTA_STATUS_VERIFYING = "ĐANG XÁC MINH...";
inline const char *OTA_STATUS_COMPLETED = "HOÀN TẤT";
inline const char *OTA_MSG_UPDATE_AVAILABLE = "Bản mới: v";
inline const char *OTA_MSG_COMPLETED = "Khởi động lại để cập nhật.";
inline const char *OTA_DOWNLOAD_BTN = "[A] Tải bản mới";
inline const char *OTA_CHECK_AGAIN_BTN = "[A] Kiểm tra lại";
inline const char *OTA_BACK_BTN = "[B] Quay lại";

// --- 15. TOAST MESSAGES ---
inline const char *TOAST_ADDED_TO_QUEUE = "Đã thêm: ";
inline const char *TOAST_REMOVED_FROM_QUEUE = "Đã bỏ: ";
inline const char *TOAST_DOWNLOAD_STOPPED = "Đã dừng: ";
inline const char *TOAST_GAME_EXISTS_DELETE = "Game đã có trên thẻ.";
inline const char *TOAST_GAME_ONLY_ON_DRIVE = "Game chỉ có trên Cloud. Không thể xóa.";
inline const char *TOAST_CONNECT_DRIVE_FIRST = "Kết nối Drive trước.";
inline const char *TOAST_DELETED_SUCCESS = "Đã xóa: ";
inline const char *TOAST_LOGOUT_SUCCESS = "Đã đăng xuất Drive.";
inline const char *TOAST_OTA_CANCELLED = "Đã hủy cập nhật.";

// --- 16. MULTI-SELECT MODE ---
inline const char *MULTI_SELECT_ENABLED = "Chế độ chọn nhiều";
inline const char *MULTI_SELECT_DISABLED = "Đã tắt chọn nhiều.";
inline const char *MULTI_SELECT_HINT = "[Y] Chọn • [L1] Tải lên • [R2] Tải về • [X] Xóa";
inline const char *MULTI_SELECT_COUNT = " đã chọn";

// --- 16B. MENU SUBTITLE ---
inline const char *MENU_SUB_PLAY = "Thư viện game";
inline const char *MENU_SUB_SYNC = "Đồng bộ dữ liệu từ Drive";
inline const char *MENU_SUB_REVERSE_SYNC = "Tải game lên Drive";
inline const char *MENU_SUB_OTA = "Kiểm tra cập nhật";
inline const char *MENU_SUB_OTA_NEW = "Có bản mới!";
inline const char *MENU_SUB_SETTINGS = "Cài đặt hệ thống";
inline const char *MENU_SUB_DIAG = "Thông tin thiết bị";
inline const char *MENU_SUB_EXIT = "Thoát ứng dụng";

// --- 16C. SEARCH ---
inline const char *SEARCH_BADGE_LOCAL = "THẺ";
inline const char *SEARCH_BADGE_CLOUD = "CLOUD";
inline const char *SEARCH_PROMPT_INPUT = "Nhập tên game...";

// --- 17. TOAST (EXTRA) ---
inline const char *TOAST_NEW_OTA_PREFIX = "Bản OTA mới: v";
inline const char *TOAST_SYNCING_DRIVE = "Đang đồng bộ Drive...";
inline const char *TOAST_SCANNING_SD = "Đang quét thẻ SD...";
inline const char *TOAST_SD_SCANNED_NO_DRIVE = "Đã quét thẻ, cần kết nối Drive.";
inline const char *TOAST_UNKNOWN_ERROR = "Lỗi không xác định.";
inline const char *TOAST_SYNC_CANCELLED = "Đã hủy đồng bộ.";
inline const char *TOAST_GOOGLE_LOGIN_SUCCESS = "Đăng nhập Google thành công!";
inline const char *TOAST_CONNECT_PERSONAL_DRIVE = "Kết nối Drive cá nhân.";

// --- 17B. BACKUP ---
inline const char *BACKUP_SUCCESS = "Sao lưu thành công!";
inline const char *BACKUP_FAILED = "Sao lưu thất bại.";

// --- 18. BACKUP & RESTORE ---
inline const char *BACKUP_TITLE = "SAO LƯU & PHỤC HỒI";
inline const char *BACKUP_EXPORT_BTN = "[A] Sao lưu";
inline const char *BACKUP_IMPORT_BTN = "[B] Phục hồi";
inline const char *BACKUP_EXPORTING = "Đang sao lưu...";
inline const char *BACKUP_IMPORTING = "Đang phục hồi...";
inline const char *BACKUP_EXPORT_SUCCESS = "Sao lưu thành công!";
inline const char *BACKUP_EXPORT_FAILED = "Sao lưu thất bại.";
inline const char *BACKUP_RESTORE_SUCCESS = "Phục hồi thành công!";
inline const char *BACKUP_RESTORE_FAILED = "Phục hồi thất bại.";
inline const char *BACKUP_NO_FILE = "Không tìm thấy file sao lưu.";

// --- 19. DASHBOARD ---
inline const char *DASH_TITLE = "ROMCLOUD";
inline const char *DASH_DEVICE_LABEL = "Thiết bị:";
inline const char *DASH_DEVICE_VAL = "TrimUI Brick Pro";
inline const char *DASH_STORAGE_LABEL = "Dung lượng:";
inline const char *DASH_STORAGE_FREE = "Còn trống ";
inline const char *DASH_SD_PREFIX = "Thẻ: ";
inline const char *DASH_DRIVE_LABEL = "Google Drive:";
inline const char *DASH_DRIVE_CONNECTED = "Đã kết nối";
inline const char *DASH_DRIVE_DISCONNECTED = "Chưa kết nối";
inline const char *DASH_PORTAL_LABEL = "Portal:";
inline const char *DASH_VERSION_LABEL = "Phiên bản:";
inline const char *DASH_NEW_VERSION_BADGE = "CẬP NHẬT";

// --- 20. GAME DETAIL ---
inline const char *GAME_LOCATION_SD_PREFIX = "Thẻ (/Roms/";
inline const char *GAME_LOCATION_DRIVE = "Cloud";
inline const char *GAME_DOWNLOADING_PREFIX = "Đang tải ";
inline const char *GAME_QUEUE_DOWNLOADING = "Đang tải, còn ";
inline const char *GAME_QUEUE_WAITING = "Hàng chờ: ";
inline const char *GAME_QUEUE_REMAINING = " game.";

// --- 21. DIALOG ---
inline const char *DIALOG_DELETE_FILE_PREFIX = "File: ";

// --- 22. BATCH DELETE ---
inline const char *MULTI_BATCH_DELETE_TITLE = "XÓA NHIỀU ROM?";
inline const char *MULTI_BATCH_DELETE_SAFE = "Bản trên Drive vẫn an toàn.";
inline const char *MULTI_BATCH_DELETE_CONFIRM = "[A] Xóa tất cả";

// --- 23. OTA (EXTRA) ---
inline const char *OTA_WAITING = "Đang chờ...";
inline const char *OTA_MSG_UP_TO_DATE = "ĐÃ LÀ BẢN MỚI NHẤT";
inline const char *OTA_NO_NEW_UPDATE = "Bạn đang sử dụng phiên bản mới nhất.";
inline const char *OTA_BTNS_CHECK_BACK = "[A] Kiểm tra  •  [B] Quay lại";
inline const char *OTA_STATUS_NEW_UPDATE = "CÓ BẢN MỚI!";
inline const char *OTA_DEV_NEW_VER_PREFIX = "Bản mới: v";
inline const char *OTA_CHANGELOG_TITLE = "Thay đổi:";
inline const char *OTA_BTN_INSTALL_NOW = "[A] Tải & Cài đặt";
inline const char *OTA_DOWNLOADING_TITLE = "ĐANG TẢI...";
inline const char *OTA_VERIFYING_FILE = "Đang xác minh file...";
inline const char *OTA_BTN_CANCEL_DOWNLOAD = "[B] Hủy tải";
inline const char *OTA_MSG_RESTART_HINT = "Tải xong! Khởi động lại để cập nhật.";
inline const char *OTA_BTN_RESTART_NOW = "[A] Khởi động lại ngay";
inline const char *OTA_MSG_FAILED = "TẢI THẤT BẠI";
inline const char *OTA_ERR_NETWORK = "Lỗi mạng. Kiểm tra Wi-Fi.";
inline const char *OTA_BTNS_RETRY_BACK = "[A] Thử lại  •  [B] Quay lại";

// --- 24. SETTINGS ---
inline const char *BACKUP_EXPORT_DESC = "Xuất sao lưu";
inline const char *BACKUP_IMPORT_DESC = "Phục hồi sao lưu";

} // namespace UiStrings
} // namespace RomCloud
