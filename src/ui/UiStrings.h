#pragma once

#include <string>

namespace RomCloud {
namespace UiStrings {

// ============================================================================
// CẤU HÌNH GIAO DIỆN — ROMCLOUD CHO TRIMUI BRICK PRO
// Chỉnh sửa chuỗi văn bản tại đây và biên dịch lại.
// ============================================================================

// --- 1. TIÊU ĐỀ CHUNG & HEADER ---
inline const char* APP_TITLE                  = "ROMCLOUD";
inline const char* APP_SUBTITLE               = "TRIMUI BRICK PRO";
inline const char* SYSTEM_SELECT_TITLE        = "CHỌN HỆ MÁY";
inline const char* HEADER_SYSTEM_SELECT       = "CHỌN HỆ MÁY";
inline const char* HEADER_SEARCH              = "TÌM KIẾM";
inline const char* HEADER_SETTINGS            = "CÀI ĐẶT";
inline const char* HEADER_WEB_CONNECT         = "KẾT NỐI DRIVE";
inline const char* HEADER_SYNC                = "ĐỒNG BỘ DRIVE";
inline const char* HEADER_DOWNLOAD            = "ĐANG TẢI ROM";
inline const char* HEADER_DIAG                = "THÔNG TIN HỆ THỐNG";
inline const char* HEADER_OTA                 = "CẬP NHẬT OTA";

// --- 2. MENU CHÍNH (MAIN MENU) ---
inline const char* MENU_PLAY                  = "THƯ VIỆN GAME";
inline const char* MENU_SYNC                  = "ĐỒNG BỘ DỮ LIỆU";
inline const char* MENU_REVERSE_SYNC          = "ĐỒNG BỘ NGƯỢC LÊN DRIVE";
inline const char* MENU_OTA                   = "CẬP NHẬT OTA";
inline const char* MENU_OTA_NEW_BADGE         = "🆕 OTA v";
inline const char* MENU_SETTINGS              = "CÀI ĐẶT";
inline const char* MENU_DIAG                  = "THÔNG TIN HỆ THỐNG";
inline const char* MENU_EXIT                  = "THOÁT";

// --- 3. THANH ĐIỀU KHIỂN & NÚT BẤM ---
inline const char* FOOTER_ENTER_SYSTEM        = "VÀO HỆ MÁY";
inline const char* FOOTER_MAIN_MENU           = "MENU CHÍNH";
inline const char* FOOTER_SYNC_DRIVE          = "ĐỒNG BỘ DRIVE";
inline const char* FOOTER_CHANGE_PAGE         = "CHUYỂN TRANG";
inline const char* FOOTER_ADD_QUEUE           = "+ HÀNG TẢI";
inline const char* FOOTER_BACK                = "QUAY LẠI";
inline const char* FOOTER_CANCEL              = "HỦY";
inline const char* FOOTER_DELETE              = "BỎ / XÓA";
inline const char* FOOTER_SYNC                = "ĐỒNG BỘ";
inline const char* FOOTER_FILTER              = "BỘ LỌC";
inline const char* FOOTER_CONFIRM             = "XÁC NHẬN";
inline const char* FOOTER_SELECT              = "CHỌN";

inline const char* BTN_ENTER_SYSTEM           = "VÀO HỆ MÁY";
inline const char* BTN_MAIN_MENU              = "MENU CHÍNH";
inline const char* BTN_SYNC_DRIVE             = "ĐỒNG BỘ DRIVE";
inline const char* BTN_CHANGE_PAGE            = "CHUYỂN TRANG";
inline const char* BTN_ADD_QUEUE              = "+ HÀNG TẢI";
inline const char* BTN_BACK                   = "QUAY LẠI";
inline const char* BTN_CANCEL                 = "HỦY";
inline const char* BTN_DELETE_QUEUE           = "BỎ KHỎI HÀNG";
inline const char* BTN_DELETE_ROM_SD          = "XÓA ROM (THẺ)";
inline const char* BTN_DELETE_ROM             = "XÓA ROM";
inline const char* BTN_CONFIRM_DELETE_SD      = "XÁC NHẬN XÓA";
inline const char* BTN_CONFIRM_DELETE         = "[A] Xóa";
inline const char* BTN_CANCEL_DELETE          = "[B] Hủy";
inline const char* BTN_SYNC                   = "ĐỒNG BỘ";
inline const char* BTN_FILTER                 = "BỘ LỌC";
inline const char* BTN_CONFIRM                = "XÁC NHẬN";
inline const char* BTN_SELECT                 = "CHỌN";
inline const char* BTN_JUMP_ALPHA             = "NHẢY CHỮ";
inline const char* BTN_SEARCH                 = "TÌM KIẾM";
inline const char* BTN_SELECT_CHAR            = "CHỌN";
inline const char* BTN_CLEAR_ALL              = "XÓA HẾT";
inline const char* BTN_DOWNLOAD               = "TẢI VỀ";
inline const char* BTN_CANCEL_ACTION          = "HỦY";
inline const char* BTN_BACK_MAIN_MENU_HINT    = "Nhấn [B] → Menu";

// --- 4. MÀN HÌNH CHỌN HỆ MÁY ---
inline const char* SYS_SELECT_TITLE           = "CHỌN HỆ MÁY";
inline const char* SYS_SELECT_SYSTEMS_LABEL   = "Hệ máy | ";
inline const char* SYS_SELECT_GAMES_LOCAL     = "Game trên thẻ | ";
inline const char* SYS_SELECT_GAMES_CLOUD     = "Game trên Cloud)";
inline const char* SYS_SELECT_SDCARD_PREFIX   = "THẺ NHỚ: ";
inline const char* SYS_SELECT_FOLDER_PREFIX   = "Thư mục: /Roms/";
inline const char* SYS_SELECT_EXT_PREFIX      = " | Định dạng: ";

// --- 5. MÀN HÌNH TÌM KIẾM ---
inline const char* SEARCH_PROMPT_MIN_CHARS    = "Nhập ít nhất 2 ký tự...";
inline const char* SEARCH_NO_RESULTS          = "Không tìm thấy kết quả.";
inline const char* SEARCH_RESULTS_SUFFIX      = " kết quả";
inline const char* SEARCH_NAV_UP_HINT         = "◀ Lên để quay lại bàn phím";
inline const char* SEARCH_NAV_DOWN_HINT        = "▼ Xuống để xem kết quả";

// --- 6. DANH SÁCH GAME & CHI TIẾT GAME ---
inline const char* GAME_LIST_EMPTY            = "Không có game phù hợp bộ lọc.";
inline const char* GAME_FILTER_HINT           = "Nhấn [SELECT] để chuyển bộ lọc (TẤT CẢ / THẺ / CLOUD)";
inline const char* FILTER_TAG_ALL             = "[SELECT] TẤT CẢ";
inline const char* FILTER_TAG_LOCAL           = "[SELECT] THẺ NHỚ";
inline const char* FILTER_TAG_CLOUD           = "[SELECT] CLOUD";
inline const char* FILTER_LABEL_ALL           = "Lọc: TẤT CẢ";
inline const char* FILTER_LABEL_LOCAL         = "Lọc: CHỈ THẺ NHỚ";
inline const char* FILTER_LABEL_CLOUD         = "Lọc: CHỈ CLOUD";
inline const char* BADGE_LOCAL                = "ĐÃ TẢI";
inline const char* BADGE_CLOUD                = "TRÊN CLOUD";
inline const char* BADGE_DOWNLOADED           = "✓ ĐÃ TẢI";
inline const char* BADGE_DOWNLOADING          = "ĐANG TẢI...";
inline const char* BADGE_DOWNLOADING_PCT      = "Đang tải... ";
inline const char* BADGE_QUEUED               = "CHỜ TẢI...";
inline const char* BADGE_DELETE_BTN           = "[X] XÓA ROM";
inline const char* BADGE_CANCEL_DL_BTN        = "[X] HỦY TẢI";
inline const char* BADGE_REMOVE_QUEUE_BTN     = "[X] BỎ KHỎI HÀNG";
inline const char* BADGE_ADD_QUEUE_BTN        = "[A] THÊM VÀO HÀNG";
inline const char* DETAIL_SYSTEM             = "Hệ máy:";
inline const char* DETAIL_LOCATION            = "Vị trí:";
inline const char* DETAIL_SIZE                = "Dung lượng:";
inline const char* DETAIL_FILENAME            = "Tên tệp:";
inline const char* DETAIL_SYS_LABEL           = "Hệ máy:";
inline const char* DETAIL_SIZE_LABEL          = "Dung lượng:";
inline const char* DETAIL_LOCATION_LABEL      = "Lưu tại:";
inline const char* DETAIL_SD_PATH_PREFIX      = "Thẻ (/Roms/";
inline const char* DETAIL_LOCAL_STORAGE       = "Thẻ nhớ MicroSD";
inline const char* DETAIL_CLOUD_STORAGE       = "Google Drive";
inline const char* QUEUE_DOWNLOADING_ACTIVE   = "Đang tải 1 game, còn ";
inline const char* QUEUE_REMAINING_SUFFIX     = " game chờ.";
inline const char* QUEUE_DOWNLOADING_EMPTY    = "Đang tải... Hàng trống.";
inline const char* QUEUE_WAITING_PREFIX       = "Hàng tải: ";

// --- 7. HỘP THOẠI XÓA ROM ---
inline const char* DELETE_TITLE               = "XÓA ROM KHỎI THẺ NHỚ?";
inline const char* DELETE_DESC                = "Thao tác này chỉ xóa file ROM khỏi thẻ SD.";
inline const char* DELETE_CLOUD_SAFE          = "Bản trên Drive vẫn an toàn.";
inline const char* DELETE_CONFIRM_BTN         = "[A] Xác nhận xóa";
inline const char* DELETE_CANCEL_BTN          = "[B] Hủy";
inline const char* DIALOG_DELETE_TITLE        = "XÁC NHẬN XÓA ROM";
inline const char* DIALOG_DELETE_FILE_LABEL   = "Tập tin: ";
inline const char* DIALOG_DELETE_PROMPT       = "Xóa ROM khỏi thẻ nhớ?";
inline const char* DIALOG_DELETE_SAFE_HINT    = "Bản trên Drive vẫn an toàn.";

// --- 8. TUYÊN BỐ BẢN QUYỀN ---
inline const char* DISCLAIMER_TITLE           = "TUYÊN BỐ BẢN QUYỀN";
inline const char* DISCLAIMER_SUBTITLE        = "Xác nhận quyền sở hữu trước khi kết nối Drive";
inline const char* DISCLAIMER_SEC1_TITLE      = "Phạm vi:";
inline const char* DISCLAIMER_SEC1_LINE1      = "RomCloud chỉ đồng bộ dữ liệu cá nhân.";
inline const char* DISCLAIMER_SEC1_LINE2      = "Không lưu trữ hay phân phối ROM.";
inline const char* DISCLAIMER_SEC2_TITLE      = "Nguồn dữ liệu:";
inline const char* DISCLAIMER_SEC2_LINE1      = "ROM lấy trực tiếp từ Google Drive của bạn.";
inline const char* DISCLAIMER_SEC2_LINE2      = "Chỉ nội dung bạn có quyền truy cập.";
inline const char* DISCLAIMER_SEC3_TITLE      = "Trách nhiệm:";
inline const char* DISCLAIMER_SEC3_LINE1      = "Bạn chịu trách nhiệm về bản quyền ROM";
inline const char* DISCLAIMER_SEC3_LINE2      = "trên tài khoản Drive của mình.";
inline const char* DISCLAIMER_SEC3_LINE3      = "";
inline const char* DISCLAIMER_AGREE           = "[A] Đồng ý & Tiếp tục";
inline const char* DISCLAIMER_DECLINE         = "[B] Từ chối & Quay lại";

// --- 9. CÀI ĐẶT HỆ THỐNG ---
inline const char* SETTING_DRIVE_STATUS       = "Google Drive:";
inline const char* SETTING_CONNECTED          = "ĐÃ KẾT NỐI";
inline const char* SETTING_DISCONNECTED       = "CHƯA KẾT NỐI";
inline const char* SETTING_LOGOUT_BTN         = "[X] Đăng xuất";
inline const char* SETTING_CONNECT_WEB_BTN    = "[A] Kết nối Web Local";
inline const char* SETTING_DRIVE_FOLDER       = "Thư mục Drive:";
inline const char* SETTING_NOT_CONFIGURED     = "Chưa thiết lập";
inline const char* SETTING_ROM_SD_FOLDER      = "ROM trên thẻ:";
inline const char* SETTING_LAST_SYNC          = "Đồng bộ cuối:";
inline const char* SETTING_NEVER_SYNCED       = "Chưa đồng bộ";
inline const char* SETTING_SQLITE_DB          = "Cơ sở dữ liệu SQLite:";
inline const char* SETTING_SCAN_MODE          = "Quét Drive:";
inline const char* SETTING_SCAN_AUTO          = "Tự động (Quét toàn bộ)";
inline const char* SETTING_WEB_PORTAL         = "Portal:";
inline const char* SETTING_COVER_CACHE        = "Bộ nhớ đệm ảnh bìa:";
inline const char* SETTING_COVER_CACHE_VAL    = "SDL2_image (tối đa 64 ảnh)";

// --- 10. KẾT NỐI QUA WEB ---
inline const char* WEB_CONNECT_TITLE          = "KẾT NỐI GOOGLE DRIVE";
inline const char* WEB_CONNECT_GUIDE_TITLE    = "KẾT NỐI TÀI KHOẢN QUA TRÌNH DUYỆT";
inline const char* WEB_CONNECT_STEP1          = "BƯỚC 1: Kết nối điện thoại/PC vào cùng Wi-Fi với TrimUI.";
inline const char* WEB_CONNECT_STEP2          = "BƯỚC 2: Mở trình duyệt (Chrome, Safari...) truy cập:";
inline const char* WEB_CONNECT_STEP3          = "BƯỚC 3: Trên trang web, bấm \"ĐĂNG NHẬP GOOGLE\" hoặc nhập liên kết thư mục Drive.";
inline const char* WEB_CONNECT_WAITING        = "* Đang chờ kết nối từ trình duyệt...";
inline const char* WEB_CONNECT_AUTO_HINT      = "Máy sẽ tự hoàn tất khi bạn đăng nhập thành công.";
inline const char* WEB_CONNECT_BACK_BTN       = "[B] Quay lại Cài đặt";
inline const char* WEB_CONNECT_SUCCESS        = "KẾT NỐI THÀNH CÔNG!";
inline const char* WEB_CONNECT_ACCOUNT_PREFIX = "Tài khoản Google:";
inline const char* WEB_CONNECT_READY          = "Kho game đã sẵn sàng để tải về.";
inline const char* WEB_CONNECT_START_BTN      = "[A] / [B] Bắt đầu sử dụng";

// --- 11. ĐỒNG BỘ GOOGLE DRIVE ---
inline const char* SYNC_TITLE                 = "ĐỒNG BỘ DỮ LIỆU CLOUD";
inline const char* SYNC_CONNECTING            = "Đang kết nối Google Drive...";
inline const char* SYNC_SCANNING              = "Đang quét danh mục game trên Drive...";
inline const char* SYNC_CANCEL_HINT           = "[B] Hủy đồng bộ";
inline const char* SYNC_API_CONNECTING        = "Đang kết nối Google Drive API...";
inline const char* SYNC_AUTH_TOKEN            = "Đang xác thực OAuth...";
inline const char* SYNC_SCANNING_GAMES        = "Đang quét danh mục game trên Drive...";
inline const char* SYNC_SEARCH_FOLDERS        = "Tìm thư mục /RomCloud hoặc /Roms...";
inline const char* SYNC_SCANNING_SYS_PREFIX   = "Đang quét hệ máy: ";
inline const char* SYNC_SYS_PREFIX            = "Hệ máy ";
inline const char* SYNC_SAVED_PREFIX          = "Đã lưu: ";
inline const char* SYNC_NEW_PREFIX            = " (Mới: ";
inline const char* SYNC_UPDATED_PREFIX        = ", Cập nhật: ";
inline const char* SYNC_CANCEL_BTN            = "[B] Hủy đồng bộ";

// --- 11B. REVERSE SYNC (UPLOAD) ---
inline const char* REVERSE_SYNC_TITLE         = "ĐỒNG BỘ NGƯỢC LÊN DRIVE";
inline const char* REVERSE_SYNC_PREPARING     = "Đang quét game trên thẻ nhớ...";
inline const char* REVERSE_SYNC_UPLOADING      = "Đang tải lên Drive...";
inline const char* REVERSE_SYNC_GAME_PROGRESS  = "Đang tải: ";
inline const char* REVERSE_SYNC_STATS          = "Đã tải: ";
inline const char* REVERSE_SYNC_SUCCESS       = "Hoàn tất! Đã tải lên ";
inline const char* REVERSE_SYNC_SUCCESS_SUF   = " game.";
inline const char* REVERSE_SYNC_FAILED         = "Thất bại: ";
inline const char* REVERSE_SYNC_CANCELLED      = "Đã hủy đồng bộ ngược.";
inline const char* REVERSE_SYNC_NO_GAMES       = "Không có game cần tải lên.";
inline const char* REVERSE_SYNC_NOT_LINKED     = "Chưa kết nối Drive. Vào Cài đặt để kết nối.";
inline const char* REVERSE_SYNC_BTN            = "[Y] Đồng bộ ngược";
inline const char* REVERSE_SYNC_CANCEL_BTN    = "[B] Hủy";
inline const char* REVERSE_SYNC_GAMES_FOUND    = "Tìm thấy ";  // + count + " game trên thẻ"
inline const char* REVERSE_SYNC_GAMES_SUF     = " game trên thẻ chưa có trên Cloud.";

// --- 12. TIẾN TRÌNH TẢI ROM ---
inline const char* DL_SYS_PREFIX              = "Hệ máy: ";
inline const char* DL_FILE_PREFIX             = " | Tập tin: ";
inline const char* DL_CONNECTING              = "Đang kết nối...";
inline const char* DL_CHECKING_FILE           = "Đang kiểm tra tập tin...";
inline const char* DL_FROM_DRIVE              = "Đang tải từ Drive...";
inline const char* DL_REMAINING_QUEUE_PREFIX  = "Còn ";
inline const char* DL_REMAINING_QUEUE_SUFFIX  = " game trong hàng chờ.";
inline const char* DL_CANCEL_BTN              = "[B] Hủy tải";

// --- 13. THÔNG TIN HỆ THỐNG ---
inline const char* DIAG_HW_DEVICE             = "Thiết bị phần cứng";
inline const char* DIAG_CPU_ARCH              = "Kiến trúc CPU";
inline const char* DIAG_OS_KERNEL             = "Hệ điều hành & Nhân";
inline const char* DIAG_RAM                   = "Bộ nhớ RAM";
inline const char* DIAG_FREE_PREFIX           = "Còn trống ";
inline const char* DIAG_TOTAL_SEPARATOR       = " / Tổng ";
inline const char* DIAG_DISPLAY               = "Màn hình";
inline const char* DIAG_SDL2_GFX              = "Thư viện đồ họa SDL2";
inline const char* DIAG_HW_ACCEL              = " (Tăng tốc phần cứng)";
inline const char* DIAG_SQLITE_DB             = "Cơ sở dữ liệu SQLite3";
inline const char* DIAG_SCHEMA_VER_PREFIX     = " (Cấu trúc v";
inline const char* DIAG_SD_STORAGE            = "Dung lượng thẻ nhớ";
inline const char* DIAG_GAMEPAD               = "Cụm phím điều khiển";
inline const char* DIAG_WIFI                  = "Kết nối Wi-Fi";
inline const char* DIAG_SAFETY                = "Độ an toàn hệ thống";
inline const char* DIAG_SAFETY_VAL            = "100% — Lưu trên thẻ nhớ (không sửa /rom, /usr, /overlay)";

// --- 14. CẬP NHẬT OTA ---
inline const char* OTA_TITLE                  = "CẬP NHẬT HỆ THỐNG OTA";
inline const char* OTA_CHECKING               = "Đang kiểm tra cập nhật từ GitHub...";
inline const char* OTA_UP_TO_DATE             = "Phiên bản hiện tại đã là mới nhất!";
inline const char* OTA_NEW_VERSION            = "Đã có phiên bản mới!";
inline const char* OTA_INSTALLING             = "Đang tải và cập nhật RomCloud...";
inline const char* OTA_SUCCESS               = "Cập nhật thành công! Nhấn [A] để khởi động lại.";
inline const char* OTA_FAILED                = "Cập nhật thất bại. Kiểm tra kết nối Wi-Fi.";
inline const char* OTA_CURRENT_VER_PREFIX     = "Phiên bản hiện tại: v";
inline const char* OTA_SOURCE_PREFIX          = "Nguồn: GitHub @";
inline const char* OTA_WAITING                = "Vui lòng đợi...";
inline const char* OTA_STATUS_UP_TO_DATE      = "ĐÃ LÀ PHIÊN BẢN MỚI NHẤT";
inline const char* OTA_MSG_UP_TO_DATE         = "Bạn đang dùng phiên bản mới nhất!";
inline const char* OTA_NO_NEW_UPDATE          = "Không có bản cập nhật mới.";
inline const char* OTA_BTNS_CHECK_BACK        = "[A] Kiểm tra lại  |  [B] Quay lại";
inline const char* OTA_STATUS_NEW_UPDATE      = "CÓ BẢN CẬP NHẬT MỚI";
inline const char* OTA_NEW_VER_PREFIX         = "Phiên bản mới: v";
inline const char* OTA_CHANGELOG_TITLE        = "Nội dung cập nhật:";
inline const char* OTA_BTN_INSTALL_NOW        = "[A] TẢI VỀ & CẬP NHẬT";
inline const char* OTA_DOWNLOADING_TITLE      = "ĐANG TẢI BẢN CẬP NHẬT...";
inline const char* OTA_VERIFYING_FILE         = "Đang kiểm tra tính toàn vẹn tập tin...";
inline const char* OTA_BTN_CANCEL_DOWNLOAD    = "[B] Hủy cập nhật";
inline const char* OTA_STATUS_COMPLETED       = "HOÀN TẤT!";
inline const char* OTA_MSG_COMPLETED          = "Cập nhật hoàn tất!";
inline const char* OTA_MSG_RESTART_HINT       = "Bản cập nhật có hiệu lực sau khi khởi động lại.";
inline const char* OTA_BTN_RESTART_NOW        = "[A] KHỞI ĐỘNG LẠI";
inline const char* OTA_STATUS_FAILED          = "THẤT BẠI";
inline const char* OTA_MSG_FAILED             = "Không thể cập nhật phần mềm.";
inline const char* OTA_ERR_NETWORK            = "Lỗi kết nối mạng hoặc GitHub.";
inline const char* OTA_BTNS_RETRY_BACK        = "[A] Thử lại  |  [B] Quay lại";

// --- 15. THÔNG BÁO TOAST ---
inline const char* TOAST_NEW_OTA_PREFIX       = "Có bản cập nhật mới: v";
inline const char* TOAST_SYNCING_DRIVE        = "Đang đồng bộ Drive...";
inline const char* TOAST_SCANNING_SD          = "Đang quét thẻ nhớ...";
inline const char* TOAST_SD_SCANNED_NO_DRIVE  = "Đã quét thẻ (chưa kết nối Drive).";
inline const char* TOAST_DOWNLOAD_COMPLETE    = "Đã tải: ";
inline const char* TOAST_UNKNOWN_ERROR        = "Lỗi không xác định.";
inline const char* TOAST_DOWNLOAD_FAILED      = "Tải thất bại: ";
inline const char* TOAST_SYNC_CANCELLED       = "Đã hủy đồng bộ Cloud.";
inline const char* TOAST_SYNC_NO_DRIVE        = "Chưa kết nối Google Drive.";
inline const char* TOAST_SYNC_COMPLETED_PRE   = "Đồng bộ xong: ";
inline const char* TOAST_SYNC_COMPLETED_SUF   = " game trong thư viện!";
inline const char* TOAST_GOOGLE_LOGIN_SUCCESS = "Đăng nhập Google thành công! [Y] đồng bộ.";
inline const char* TOAST_GAME_EXISTS_DELETE   = "Game đã có trên thẻ. [X] xóa.";
inline const char* TOAST_ALREADY_IN_QUEUE     = " đã có trong hàng tải.";
inline const char* TOAST_ADDED_TO_QUEUE       = "Đã thêm vào hàng: ";
inline const char* TOAST_CONNECT_DRIVE_FIRST  = "Kết nối Drive trước!";
inline const char* TOAST_DOWNLOAD_STOPPED     = "Đã dừng tải: ";
inline const char* TOAST_REMOVED_FROM_QUEUE   = "Đã bỏ khỏi hàng: ";
inline const char* TOAST_GAME_ONLY_ON_DRIVE  = "Game chưa tải về (chỉ có trên Drive).";
inline const char* TOAST_JUMP_ALPHA_PRE       = "Nhảy đến chữ: [ ";
inline const char* TOAST_DELETED_PRE          = "Đã xóa \"";
inline const char* TOAST_DELETED_SUF          = "\" khỏi thẻ.";
inline const char* TOAST_UNLINK_SUCCESS       = "Đã hủy liên kết Google Drive.";
inline const char* TOAST_LOGOUT_SUCCESS       = "Đã đăng xuất khỏi Drive.";
inline const char* TOAST_OTA_CANCELLED        = "Đã hủy cập nhật.";


// --- 16. GIAO DIỆN HIỆN ĐẠI (BORDERLESS & DASHBOARD) ---
inline const char* MENU_SUB_PLAY             = "Khám phá & tải game về thẻ nhớ";
inline const char* MENU_SUB_SYNC             = "Đồng bộ thư viện với Google Drive";
inline const char* MENU_SUB_REVERSE_SYNC     = "Tải game từ thẻ nhớ lên Google Drive";
inline const char* MENU_SUB_OTA_NEW          = "Bản nâng cấp mới đã sẵn sàng tải";
inline const char* MENU_SUB_OTA              = "Kiểm tra phiên bản & cập nhật OTA";
inline const char* MENU_SUB_SETTINGS         = "Cấu hình tài khoản & thư mục ROM";
inline const char* MENU_SUB_DIAG             = "Thông số phần cứng, RAM & mạng";
inline const char* MENU_SUB_EXIT             = "Quay về giao diện TrimUI";

inline const char* DASH_TITLE                = "TRẠNG THÁI HỆ THỐNG";
inline const char* DASH_DEVICE_LABEL         = "Thiết bị";
inline const char* DASH_DEVICE_VAL           = "TrimUI Smart Pro (ARM64)";
inline const char* DASH_STORAGE_LABEL        = "Bộ nhớ & ROMs";
inline const char* DASH_STORAGE_FREE         = " trống / ";
inline const char* DASH_SD_PREFIX            = "Thẻ nhớ: ";
inline const char* DASH_DRIVE_LABEL          = "Google Drive Sync";
inline const char* DASH_DRIVE_CONNECTED      = "Đã kết nối";
inline const char* DASH_DRIVE_DISCONNECTED   = "Chưa kết nối tài khoản";
inline const char* DASH_PORTAL_LABEL         = "Web Manager Portal";
inline const char* DASH_VERSION_LABEL        = "Phiên bản";
inline const char* DASH_NEW_VERSION_BADGE    = "CÓ BẢN MỚI";

inline const char* SYS_BADGE_LOCAL_PREFIX    = "THẺ NHỚ: ";
inline const char* SYS_BADGE_CLOUD_PREFIX    = "CLOUD: ";
inline const char* SYS_DIR_PREFIX            = "Thư mục: /Roms/";
inline const char* SYS_EXT_PREFIX            = "  |  Định dạng: ";

inline const char* GAME_LOCATION_SD_PREFIX   = "Thẻ nhớ (/Roms/";
inline const char* GAME_LOCATION_DRIVE       = "Google Drive Cloud";
inline const char* GAME_DOWNLOADING_PREFIX   = "ĐANG TẢI... ";
inline const char* GAME_QUEUE_DOWNLOADING    = "Đang tải 1 game, còn ";
inline const char* GAME_QUEUE_REMAINING      = " game chờ.";
inline const char* GAME_QUEUE_WAITING        = "Hàng tải: ";

inline const char* SEARCH_PROMPT_INPUT       = "Nhập từ khóa tìm kiếm...";
inline const char* SEARCH_BADGE_LOCAL        = "THẺ NHỚ";
inline const char* SEARCH_BADGE_CLOUD        = "CLOUD";

inline const char* DIALOG_DELETE_FILE_PREFIX = "Tập tin: ";

inline const char* OTA_DEV_CURRENT_VER       = "Phiên bản hiện tại trên máy: v";
inline const char* OTA_DEV_SOURCE_PREFIX     = "Nguồn phát hành: GitHub @";
inline const char* OTA_DEV_NEW_VER_PREFIX    = "Phiên bản mới: v";

inline const char* DIAG_VAL_64BIT            = " (64-bit Little Endian)";
inline const char* DIAG_VAL_HW_ACCEL         = " (Tăng tốc phần cứng)";
inline const char* DIAG_VAL_SCHEMA_PREFIX    = " (Phiên bản cấu trúc v";

// --- 17. BACKUP & RESTORE ---
inline const char* BACKUP_TITLE              = "SAO LƯU & PHỤC HỒI";
inline const char* BACKUP_EXPORT_BTN         = "Sao lưu cài đặt";
inline const char* BACKUP_IMPORT_BTN        = "Phục hồi cài đặt";
inline const char* BACKUP_EXPORT_DESC        = "Xuất cài đặt ra file JSON";
inline const char* BACKUP_IMPORT_DESC        = "Nhập cài đặt từ file backup";
inline const char* BACKUP_EXPORTING          = "Đang sao lưu...";
inline const char* BACKUP_IMPORTING          = "Đang phục hồi...";
inline const char* BACKUP_SUCCESS            = "Sao lưu thành công!";
inline const char* BACKUP_FAILED             = "Sao lưu thất bại.";
inline const char* BACKUP_RESTORE_SUCCESS    = "Phục hồi thành công!";
inline const char* BACKUP_RESTORE_FAILED     = "Phục hồi thất bại.";
inline const char* BACKUP_NO_FILE            = "Không tìm thấy file backup.";
inline const char* BACKUP_LAST_EXPORT        = "Backup gần nhất:";
inline const char* BACKUP_FILE_SAVED         = "Đã lưu:";
inline const char* BACKUP_PRESS_BACK         = "[B] Quay lại";

// --- 18. MULTI-SELECT MODE ---
inline const char* MULTI_SELECT_ENABLED      = "Đã bật chọn nhiều";
inline const char* MULTI_SELECT_DISABLED     = "Đã tắt chọn nhiều";
inline const char* MULTI_SELECT_HINT         = "Dùng A/X để chọn/bỏ chọn game";
inline const char* MULTI_SELECT_COUNT_PRE    = "Đã chọn: ";
inline const char* MULTI_SELECT_COUNT_SUF    = " game";
inline const char* MULTI_BATCH_DELETE_TITLE  = "XÓA NHIỀU GAME?";
inline const char* MULTI_BATCH_DELETE_PROMPT = "Bạn muốn xóa các game đã chọn?";
inline const char* MULTI_BATCH_DELETE_SAFE   = "Bản trên Drive vẫn an toàn.";
inline const char* MULTI_BATCH_DELETE_CONFIRM = "[A] Xóa tất cả";
inline const char* MULTI_BATCH_QUEUE_TITLE   = "THÊM VÀO HÀNG TẢI";
inline const char* MULTI_BATCH_QUEUE_PROMPT  = "Thêm các game đã chọn vào hàng chờ?";
inline const char* MULTI_BATCH_QUEUE_CONFIRM = "[A] Thêm vào hàng";
} // namespace UiStrings
} // namespace RomCloud
