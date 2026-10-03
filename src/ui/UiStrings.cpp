#include "ui/UiStrings.h"

namespace RomCloud {
namespace UiStrings {

// ============================================================================
// CẤU HÌNH GIAO DIỆN — ROMCLOUD CHO TRIMUI BRICK PRO
// Chỉnh sửa chuỗi văn bản tại đây và biên dịch lại.
// ============================================================================

// --- 1. TIÊU ĐỀ CHUNG & HEADER ---
const char *APP_TITLE = "ROMCLOUD";
const char *APP_SUBTITLE = "TRIMUI BRICK PRO";
const char *SYSTEM_SELECT_TITLE = "CHỌN HỆ MÁY";
const char *HEADER_SYSTEM_SELECT = "CHỌN HỆ MÁY";
const char *HEADER_SEARCH = "TÌM KIẾM";
const char *HEADER_SETTINGS = "CÀI ĐẶT";
const char *HEADER_WEB_CONNECT = "KẾT NỐI DRIVE";
const char *HEADER_SYNC = "ĐỒNG BỘ";
const char *HEADER_DOWNLOAD = "ĐANG TẢI";
const char *HEADER_DIAG = "THÔNG TIN";
const char *HEADER_OTA = "CẬP NHẬT";

// --- 2. MENU CHÍNH (MAIN MENU) ---
const char *MENU_PLAY = "THƯ VIỆN GAME";
const char *MENU_IPTV = "XEM TV";
const char *MENU_SYNC = "ĐỒNG BỘ";
const char *MENU_REVERSE_SYNC = "TẢI LÊN DRIVE";
const char *MENU_OTA = "CẬP NHẬT";
const char *MENU_OTA_NEW_BADGE = "NEW OTA v";
const char *MENU_SETTINGS = "CÀI ĐẶT";
const char *MENU_DIAG = "THÔNG TIN";
const char *MENU_EXIT = "THOÁT";

// --- IPTV STRINGS ---
const char *IPTV_TITLE = "XEM TV";
const char *IPTV_NO_CHANNELS = "Không có kênh TV";
const char *IPTV_LOADING = "Đang tải danh sách...";
const char *IPTV_NOW_PLAYING = "Đang phát:";

// --- 3. THANH ĐIỀU KHIỂN & NÚT BẤM ---
const char *FOOTER_ENTER_SYSTEM = "VÀO";
const char *FOOTER_MAIN_MENU = "Lùi";
const char *FOOTER_SYNC_DRIVE = "ĐỒNG BỘ";
const char *FOOTER_CHANGE_PAGE = "TRANG";
const char *FOOTER_ADD_QUEUE = "+HÀNG";
const char *FOOTER_BACK = "QUAY LẠI";
const char *FOOTER_CANCEL = "HỦY";
const char *FOOTER_DELETE = "XÓA";
const char *FOOTER_SYNC = "ĐỒNG BỘ";
const char *FOOTER_FILTER = "LỌC";
const char *FOOTER_CONFIRM = "XÁC NHẬN";
const char *FOOTER_SELECT = "CHỌN";

const char *BTN_ENTER_SYSTEM = "VÀO";
const char *BTN_MAIN_MENU = "Lùi";
const char *BTN_SYNC_DRIVE = "ĐỒNG BỘ";
const char *BTN_CHANGE_PAGE = "CHUYỂN TRANG";
const char *BTN_ADD_QUEUE = "+ HÀNG TẢI";
const char *BTN_BACK = "QUAY LẠI";
const char *BTN_CANCEL = "HỦY";
const char *BTN_DELETE_QUEUE = "BỎ HÀNG";
const char *BTN_DELETE_ROM_SD = "Xóa";
const char *BTN_DELETE_ROM = "Xóa";
const char *BTN_CONFIRM_DELETE_SD = "XÁC NHẬN XÓA";
const char *BTN_CONFIRM_DELETE = "[A] Xóa";
const char *BTN_CANCEL_DELETE = "[B] Hủy";
const char *BTN_SYNC = "ĐỒNG BỘ";
const char *BTN_FILTER = "BỘ LỌC";
const char *BTN_CONFIRM = "XÁC NHẬN";
const char *BTN_SELECT = "CHỌN";
const char *BTN_JUMP_ALPHA = "Nhảy";
const char *BTN_SEARCH = "Tìm";
const char *BTN_SELECT_CHAR = "CHỌN";
const char *BTN_CLEAR_ALL = "Xóa hết";
const char *BTN_DOWNLOAD = "Tải";
const char *BTN_CANCEL_ACTION = "HỦY";
const char *BTN_BACK_MAIN_MENU_HINT = "[B] Menu";

// --- 4. MÀN HÌNH CHỌN HỆ MÁY ---
const char *SYS_SELECT_TITLE = "CHỌN HỆ MÁY";
const char *SYS_SELECT_SYSTEMS_LABEL = "Hệ máy | ";
const char *SYS_SELECT_GAMES_LOCAL = "Thẻ | ";
const char *SYS_SELECT_GAMES_CLOUD = "Cloud)";
const char *SYS_SELECT_SDCARD_PREFIX = "THẺ: ";
const char *SYS_SELECT_FOLDER_PREFIX = "/Roms/";
const char *SYS_SELECT_EXT_PREFIX = " • ";

// --- 5. MÀN HÌNH TÌM KIẾM ---
const char *SEARCH_PROMPT_MIN_CHARS = "Nhập ít nhất 2 ký tự...";
const char *SEARCH_NO_RESULTS = "Không tìm thấy.";
const char *SEARCH_RESULTS_SUFFIX = " kết quả";
const char *SEARCH_NAV_UP_HINT = "◀ Lên bàn phím";
const char *SEARCH_NAV_DOWN_HINT = "▼ Xem kết quả";

// --- 6. DANH SÁCH GAME & CHI TIẾT GAME ---
const char *GAME_LIST_EMPTY = "Không có game.";
const char *GAME_FILTER_HINT = "[SELECT] Đổi bộ lọc";
const char *FILTER_TAG_ALL = "TẤT CẢ";
const char *FILTER_TAG_LOCAL = "THẺ NHỚ";
const char *FILTER_TAG_CLOUD = "CLOUD";
const char *FILTER_LABEL_ALL = "Lọc: TẤT CẢ";
const char *FILTER_LABEL_LOCAL = "Lọc: THẺ NHỚ";
const char *FILTER_LABEL_CLOUD = "Lọc: CLOUD";
const char *BADGE_LOCAL = "THẺ";
const char *BADGE_CLOUD = "CLOUD";
const char *BADGE_DOWNLOADED = "✓ ĐÃ TẢI";
const char *BADGE_DOWNLOADING = "ĐANG TẢI...";
const char *BADGE_DOWNLOADING_PCT = "Tải ";
const char *BADGE_QUEUED = "CHỜ";
const char *BADGE_DELETE_BTN = "[X] XÓA";
const char *BADGE_CANCEL_DL_BTN = "[X] HỦY";
const char *BADGE_REMOVE_QUEUE_BTN = "[X] BỎ";
const char *BADGE_ADD_QUEUE_BTN = "[A] TẢI";
const char *DETAIL_SYSTEM = "Hệ máy:";
const char *DETAIL_LOCATION = "Vị trí:";
const char *DETAIL_SIZE = "Dung lượng:";
const char *DETAIL_FILENAME = "Tên file:";
const char *DETAIL_SYS_LABEL = "Hệ máy:";
const char *DETAIL_SIZE_LABEL = "Dung lượng:";
const char *DETAIL_LOCATION_LABEL = "Lưu tại:";
const char *DETAIL_SD_PATH_PREFIX = "Thẻ (/Roms/";
const char *DETAIL_LOCAL_STORAGE = "Thẻ nhớ MicroSD";
const char *DETAIL_CLOUD_STORAGE = "Google Drive";
const char *QUEUE_DOWNLOADING_ACTIVE = "Đang tải, còn ";
const char *QUEUE_REMAINING_SUFFIX = " game chờ.";
const char *QUEUE_DOWNLOADING_EMPTY = "Hàng trống.";
const char *QUEUE_WAITING_PREFIX = "Hàng: ";

// --- 7. HỘP THOẠI XÓA ROM ---
const char *DELETE_TITLE = "XÓA ROM?";
const char *DELETE_DESC = "Xóa file khỏi thẻ SD.";
const char *DELETE_CLOUD_SAFE = "Bản trên Drive vẫn an toàn.";
const char *DELETE_CONFIRM_BTN = "[A] Xóa";
const char *DELETE_CANCEL_BTN = "[B] Hủy";
const char *DIALOG_DELETE_TITLE = "XÁC NHẬN XÓA";
const char *DIALOG_DELETE_FILE_LABEL = "File: ";
const char *DIALOG_DELETE_PROMPT = "Xóa ROM khỏi thẻ nhớ?";
const char *DIALOG_DELETE_SAFE_HINT = "Bản Drive vẫn an toàn.";

// --- 8. TUYÊN BỐ BẢN QUYỀN ---
const char *DISCLAIMER_TITLE = "XÁC NHẬN";
const char *DISCLAIMER_SUBTITLE = "Kiểm tra quyền sở hữu trước khi kết nối";
const char *DISCLAIMER_SEC1_TITLE = "Phạm vi:";
const char *DISCLAIMER_SEC1_LINE1 = "RomCloud chỉ đồng bộ dữ liệu cá nhân.";
const char *DISCLAIMER_SEC1_LINE2 = "Không lưu trữ hay phân phối ROM.";
const char *DISCLAIMER_SEC2_TITLE = "Nguồn dữ liệu:";
const char *DISCLAIMER_SEC2_LINE1 = "ROM từ Google Drive của bạn.";
const char *DISCLAIMER_SEC2_LINE2 = "Chỉ nội dung bạn có quyền truy cập.";
const char *DISCLAIMER_SEC3_TITLE = "Trách nhiệm:";
const char *DISCLAIMER_SEC3_LINE1 = "Bạn chịu trách nhiệm về bản quyền";
const char *DISCLAIMER_SEC3_LINE2 = "ROM trên tài khoản Drive của mình.";
const char *DISCLAIMER_SEC3_LINE3 = "";
const char *DISCLAIMER_AGREE = "[A] Đồng ý";
const char *DISCLAIMER_DECLINE = "[B] Từ chối";

// --- 9. CÀI ĐẶT HỆ THỐNG ---
const char *SETTING_DRIVE_STATUS = "Google Drive:";
const char *SETTING_CONNECTED = "ĐÃ KẾT NỐI";
const char *SETTING_DISCONNECTED = "CHƯA KẾT NỐI";
const char *SETTING_LOGOUT_BTN = "[X] Đăng xuất";
const char *SETTING_CONNECT_WEB_BTN = "[A] Kết nối";
const char *SETTING_DRIVE_FOLDER = "Thư mục Drive:";
const char *SETTING_NOT_CONFIGURED = "Chưa thiết lập";
const char *SETTING_ROM_SD_FOLDER = "ROM trên thẻ:";
const char *SETTING_LAST_SYNC = "Đồng bộ cuối:";
const char *SETTING_NEVER_SYNCED = "Chưa đồng bộ";
const char *SETTING_SQLITE_DB = "Cơ sở dữ liệu:";
const char *SETTING_SCAN_MODE = "Quét Drive:";
const char *SETTING_SCAN_AUTO = "Tự động";
const char *SETTING_WEB_PORTAL = "Portal:";
const char *SETTING_COVER_CACHE = "Bộ nhớ đệm ảnh:";
const char *SETTING_COVER_CACHE_VAL = "SDL2_image (tối đa 64 ảnh)";

// --- 10. KẾT NỐI QUA WEB ---
const char *WEB_CONNECT_TITLE = "KẾT NỐI DRIVE";
const char *WEB_CONNECT_GUIDE_TITLE = "KẾT NỐI TÀI KHOẢN";
const char *WEB_CONNECT_STEP1 = "BƯỚC 1: Kết nối điện thoại/PC cùng Wi-Fi.";
const char *WEB_CONNECT_STEP2 = "BƯỚC 2: Mở trình duyệt truy cập:";
const char *WEB_CONNECT_STEP3 =
    "BƯỚC 3: Đăng nhập Google hoặc nhập liên kết Drive.";
const char *WEB_CONNECT_WAITING = "* Đang chờ kết nối...";
const char *WEB_CONNECT_AUTO_HINT = "Tự động hoàn tất khi đăng nhập.";
const char *WEB_CONNECT_BACK_BTN = "[B] Quay lại";
const char *WEB_CONNECT_SUCCESS = "KẾT NỐI THÀNH CÔNG!";
const char *WEB_CONNECT_ACCOUNT_PREFIX = "Tài khoản:";
const char *WEB_CONNECT_READY = "Kho game sẵn sàng tải về.";
const char *WEB_CONNECT_START_BTN = "[A] Bắt đầu";

// --- 11. ĐỒNG BỘ GOOGLE DRIVE ---
const char *SYNC_TITLE = "ĐỒNG BỘ";
const char *SYNC_CONNECTING = "Đang kết nối...";
const char *SYNC_SCANNING = "Đang quét game trên Drive...";
const char *SYNC_CANCEL_HINT = "[B] Hủy";
const char *SYNC_API_CONNECTING = "Đang kết nối API...";
const char *SYNC_AUTH_TOKEN = "Đang xác thực...";
const char *SYNC_SCANNING_GAMES = "Đang quét game...";
const char *SYNC_SEARCH_FOLDERS = "Tìm thư mục RomCloud...";
const char *SYNC_SCANNING_SYS_PREFIX = "Quét: ";
const char *SYNC_SYS_PREFIX = "Hệ máy ";
const char *SYNC_SAVED_PREFIX = "Đã lưu: ";
const char *SYNC_NEW_PREFIX = " (Mới: ";
const char *SYNC_UPDATED_PREFIX = ", Cập nhật: ";
const char *SYNC_CANCEL_BTN = "[B] Hủy";

// --- 11B. REVERSE SYNC (UPLOAD) ---
const char *REVERSE_SYNC_TITLE = "TẢI LÊN DRIVE";
const char *REVERSE_SYNC_PREPARING = "Đang quét game trên thẻ...";
const char *REVERSE_SYNC_UPLOADING = "Đang tải lên...";
const char *REVERSE_SYNC_GAME_PROGRESS = "Đang tải: ";
const char *REVERSE_SYNC_STATS = "Tiến độ: ";
const char *REVERSE_SYNC_SUCCESS = "Hoàn tất!";
const char *REVERSE_SYNC_SUCCESS_SUF = " game đã tải lên.";
const char *REVERSE_SYNC_FAILED = "Thất bại: ";
const char *REVERSE_SYNC_CANCELLED = "Đã hủy.";
const char *REVERSE_SYNC_NO_GAMES = "Không có game cần tải lên.";
const char *REVERSE_SYNC_NOT_LINKED = "Chưa kết nối Drive. Vào Cài đặt.";
const char *REVERSE_SYNC_BTN = "[Y] Tải lên";
const char *REVERSE_SYNC_CANCEL_BTN = "[B] Hủy";
const char *REVERSE_SYNC_GAMES_FOUND = "Tìm thấy ";
const char *REVERSE_SYNC_GAMES_SUF = " game chưa có trên Cloud.";

// --- 12. TIẾN TRÌNH TẢI ROM ---
const char *DL_SYS_PREFIX = "Hệ máy: ";
const char *DL_FILE_PREFIX = " • File: ";
const char *DL_CONNECTING = "Đang kết nối...";
const char *DL_CHECKING_FILE = "Kiểm tra file...";
const char *DL_FROM_DRIVE = "Đang tải từ Drive...";
const char *DL_REMAINING_QUEUE_PREFIX = "Còn ";
const char *DL_REMAINING_QUEUE_SUFFIX = " game trong hàng.";
const char *DL_CANCEL_BTN = "[B] Hủy";

// --- 13. THÔNG TIN HỆ THỐNG ---
const char *DIAG_HW_DEVICE = "Thiết bị";
const char *DIAG_CPU_ARCH = "CPU";
const char *DIAG_OS_KERNEL = "Hệ điều hành";
const char *DIAG_RAM = "Bộ nhớ RAM";
const char *DIAG_FREE_PREFIX = "Còn trống ";
const char *DIAG_TOTAL_SEPARATOR = " / ";
const char *DIAG_DISPLAY = "Màn hình";
const char *DIAG_SDL2_GFX = "Đồ họa SDL2";
const char *DIAG_HW_ACCEL = " (Tăng tốc)";
const char *DIAG_SQLITE_DB = "Cơ sở dữ liệu";
const char *DIAG_SCHEMA_VER_PREFIX = " (Cấu trúc v";
const char *DIAG_SD_STORAGE = "Dung lượng thẻ";
const char *DIAG_GAMEPAD = "Phím điều khiển";
const char *DIAG_WIFI = "Wi-Fi";
const char *DIAG_SAFETY = "Độ an toàn";
const char *DIAG_SAFETY_VAL = "100% — Không sửa hệ thống";
const char *DIAG_VAL_64BIT = " (64-bit)";
const char *DIAG_VAL_HW_ACCEL = " (Tăng tốc)";
const char *DIAG_VAL_SCHEMA_PREFIX = " • Cấu trúc v";

// --- 14. CẬP NHẬT OTA ---
const char *OTA_TITLE = "CẬP NHẬT";
const char *OTA_CHECKING = "Đang kiểm tra...";
const char *OTA_DEV_CURRENT_VER = "Phiên bản hiện tại: v";
const char *OTA_DEV_SOURCE_PREFIX = "Nguồn: github.com/";
const char *OTA_STATUS_IDLE = "Nhấn [A] để kiểm tra cập nhật.";
const char *OTA_STATUS_CHECKING = "Đang kiểm tra...";
const char *OTA_STATUS_UPDATE_AVAILABLE = "CÓ BẢN MỚI!";
const char *OTA_STATUS_UP_TO_DATE = "ĐÃ LÀ BẢN MỚI NHẤT";
const char *OTA_STATUS_FAILED = "KIỂM TRA THẤT BẠI";
const char *OTA_STATUS_DOWNLOADING = "ĐANG TẢI...";
const char *OTA_STATUS_VERIFYING = "ĐANG XÁC MINH...";
const char *OTA_STATUS_COMPLETED = "HOÀN TẤT";
const char *OTA_MSG_UPDATE_AVAILABLE = "Bản mới: v";
const char *OTA_MSG_COMPLETED = "Khởi động lại để cập nhật.";
const char *OTA_DOWNLOAD_BTN = "[A] Tải bản mới";
const char *OTA_CHECK_AGAIN_BTN = "[A] Kiểm tra lại";
const char *OTA_BACK_BTN = "[B] Quay lại";

// --- 15. TOAST MESSAGES ---
const char *TOAST_ADDED_TO_QUEUE = "Đã thêm: ";
const char *TOAST_REMOVED_FROM_QUEUE = "Đã bỏ: ";
const char *TOAST_DOWNLOAD_STOPPED = "Đã dừng: ";
const char *TOAST_GAME_EXISTS_DELETE = "Game đã có trên thẻ.";
const char *TOAST_GAME_ONLY_ON_DRIVE = "Game chỉ có trên Cloud. Không thể xóa.";
const char *TOAST_CONNECT_DRIVE_FIRST = "Kết nối Drive trước.";
const char *TOAST_DELETED_SUCCESS = "Đã xóa: ";
const char *TOAST_LOGOUT_SUCCESS = "Đã đăng xuất Drive.";
const char *TOAST_OTA_CANCELLED = "Đã hủy cập nhật.";

// --- 16. MULTI-SELECT MODE ---
const char *MULTI_SELECT_ENABLED = "Chế độ chọn nhiều";
const char *MULTI_SELECT_DISABLED = "Đã tắt chọn nhiều.";
const char *MULTI_SELECT_HINT =
    "[Y] Chọn • [L1] Tải lên • [R2] Tải về • [X] Xóa";
const char *MULTI_SELECT_COUNT = " đã chọn";

// --- 16B. MENU SUBTITLE ---
const char *MENU_SUB_PLAY = "Thư viện game";
const char *MENU_SUB_SYNC = "Đồng bộ dữ liệu từ Drive";
const char *MENU_SUB_REVERSE_SYNC = "Tải game lên Drive";
const char *MENU_SUB_OTA = "Kiểm tra cập nhật";
const char *MENU_SUB_OTA_NEW = "Có bản mới!";
const char *MENU_SUB_SETTINGS = "Cài đặt hệ thống";
const char *MENU_SUB_DIAG = "Thông tin thiết bị";
const char *MENU_SUB_EXIT = "Thoát ứng dụng";

// --- 16C. SEARCH ---
const char *SEARCH_BADGE_LOCAL = "THẺ";
const char *SEARCH_BADGE_CLOUD = "CLOUD";
const char *SEARCH_PROMPT_INPUT = "Nhập tên game...";

// --- 17. TOAST (EXTRA) ---
const char *TOAST_NEW_OTA_PREFIX = "Bản OTA mới: v";
const char *TOAST_SYNCING_DRIVE = "Đang đồng bộ Drive...";
const char *TOAST_SCANNING_SD = "Đang quét thẻ SD...";
const char *TOAST_SD_SCANNED_NO_DRIVE = "Đã quét thẻ, cần kết nối Drive.";
const char *TOAST_UNKNOWN_ERROR = "Lỗi không xác định.";
const char *TOAST_SYNC_CANCELLED = "Đã hủy đồng bộ.";
const char *TOAST_GOOGLE_LOGIN_SUCCESS = "Đăng nhập Google thành công!";
const char *TOAST_CONNECT_PERSONAL_DRIVE = "Kết nối Drive cá nhân.";

// --- 17B. BACKUP ---
const char *BACKUP_SUCCESS = "Sao lưu thành công!";
const char *BACKUP_FAILED = "Sao lưu thất bại.";

// --- 18. BACKUP & RESTORE ---
const char *BACKUP_TITLE = "SAO LƯU & PHỤC HỒI";
const char *BACKUP_EXPORT_BTN = "[A] Sao lưu";
const char *BACKUP_IMPORT_BTN = "[B] Phục hồi";
const char *BACKUP_EXPORTING = "Đang sao lưu...";
const char *BACKUP_IMPORTING = "Đang phục hồi...";
const char *BACKUP_EXPORT_SUCCESS = "Sao lưu thành công!";
const char *BACKUP_EXPORT_FAILED = "Sao lưu thất bại.";
const char *BACKUP_RESTORE_SUCCESS = "Phục hồi thành công!";
const char *BACKUP_RESTORE_FAILED = "Phục hồi thất bại.";
const char *BACKUP_NO_FILE = "Không tìm thấy file sao lưu.";

// --- 19. DASHBOARD ---
const char *DASH_TITLE = "ROMCLOUD";
const char *DASH_DEVICE_LABEL = "Thiết bị:";
const char *DASH_DEVICE_VAL = "TrimUI Brick Pro";
const char *DASH_STORAGE_LABEL = "Dung lượng:";
const char *DASH_STORAGE_FREE = "Còn trống ";
const char *DASH_SD_PREFIX = "Thẻ: ";
const char *DASH_DRIVE_LABEL = "Google Drive:";
const char *DASH_DRIVE_CONNECTED = "Đã kết nối";
const char *DASH_DRIVE_DISCONNECTED = "Chưa kết nối";
const char *DASH_PORTAL_LABEL = "Portal:";
const char *DASH_VERSION_LABEL = "Phiên bản:";
const char *DASH_NEW_VERSION_BADGE = "CẬP NHẬT";

// --- 20. GAME DETAIL ---
const char *GAME_LOCATION_SD_PREFIX = "Thẻ (/Roms/";
const char *GAME_LOCATION_DRIVE = "Cloud";
const char *GAME_DOWNLOADING_PREFIX = "Đang tải ";
const char *GAME_QUEUE_DOWNLOADING = "Đang tải, còn ";
const char *GAME_QUEUE_WAITING = "Hàng chờ: ";
const char *GAME_QUEUE_REMAINING = " game.";

// --- 21. DIALOG ---
const char *DIALOG_DELETE_FILE_PREFIX = "File: ";

// --- 22. BATCH DELETE ---
const char *MULTI_BATCH_DELETE_TITLE = "XÓA NHIỀU ROM?";
const char *MULTI_BATCH_DELETE_SAFE = "Bản trên Drive vẫn an toàn.";
const char *MULTI_BATCH_DELETE_CONFIRM = "[A] Xóa tất cả";

// --- 23. OTA (EXTRA) ---
const char *OTA_WAITING = "Đang chờ...";
const char *OTA_MSG_UP_TO_DATE = "ĐÃ LÀ BẢN MỚI NHẤT";
const char *OTA_NO_NEW_UPDATE = "Bạn đang sử dụng phiên bản mới nhất.";
const char *OTA_BTNS_CHECK_BACK = "[A] Kiểm tra  •  [B] Quay lại";
const char *OTA_STATUS_NEW_UPDATE = "CÓ BẢN MỚI!";
const char *OTA_DEV_NEW_VER_PREFIX = "Bản mới: v";
const char *OTA_CHANGELOG_TITLE = "Thay đổi:";
const char *OTA_BTN_INSTALL_NOW = "[A] Tải & Cài đặt";
const char *OTA_DOWNLOADING_TITLE = "ĐANG TẢI...";
const char *OTA_VERIFYING_FILE = "Đang xác minh file...";
const char *OTA_BTN_CANCEL_DOWNLOAD = "[B] Hủy tải";
const char *OTA_MSG_RESTART_HINT = "Tải xong! Khởi động lại để cập nhật.";
const char *OTA_BTN_RESTART_NOW = "[A] Khởi động lại ngay";
const char *OTA_MSG_FAILED = "TẢI THẤT BẠI";
const char *OTA_ERR_NETWORK = "Lỗi mạng. Kiểm tra Wi-Fi.";
const char *OTA_BTNS_RETRY_BACK = "[A] Thử lại  •  [B] Quay lại";

// --- 24. SETTINGS ---
const char *BACKUP_EXPORT_DESC = "Xuất sao lưu";
const char *BACKUP_IMPORT_DESC = "Phục hồi sao lưu";

// --- ABOUT ---
const char *ABOUT_AUTHOR = "Tác giả: bun2it";
const char *ABOUT_DONATE_LINE1 =
    "Hãy mời tác giả 1 ly cà phê nếu thấy app hữu ích.";
const char *ABOUT_DONATE_LINE2 = "Quét mã để chuyển khoản, MoMo, Zalopay.";
const char *ABOUT_LETTER_TITLE = "THƯ NGỎ";
const char *ABOUT_NOTES_FALLBACK = "Không có ghi chú.";

// --- DIAG ---
const char *DIAG_DEVICE_ID_LABEL = "Báo lỗi tự động:";

// --- INFO TAB ---
const char *INFO_TAB_ABOUT = "Giới thiệu";
const char *INFO_TAB_SYSTEM = "Hệ thống";

// --- OTA ---
const char *OTA_CHANGELOG_HEADER = "Cập nhật - Tính năng mới";

// --- SETTINGS TAB ---
const char *SETTINGS_TAB_CONFIG = "Cấu hình";
const char *SETTINGS_TAB_PREFS = "Tùy chọn";
const char *SETTINGS_TAB_UPDATE = "Cập nhật";
} // namespace UiStrings
} // namespace RomCloud
