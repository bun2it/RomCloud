#pragma once

#include <string>

namespace RomCloud {
namespace UiStrings {

// ============================================================================
// CẤU HÌNH TOÀN BỘ CÂU CHỮ GIAO DIỆN (UI STRINGS) CHO MÁY TRIMUI BRICK PRO
// Bạn có thể chỉnh sửa trực tiếp các chuỗi văn bản tại đây và biên dịch lại ứng dụng!
// ============================================================================

// --- 1. TIÊU ĐỀ CHUNG & HEADER ---
inline const char* APP_TITLE                  = "ROMCLOUD";
inline const char* APP_SUBTITLE               = "TRIMUI BRICK PRO";
inline const char* SYSTEM_SELECT_TITLE        = "CHỌN HỆ MÁY";
inline const char* HEADER_SYSTEM_SELECT       = "CHỌN HỆ MÁY";
inline const char* HEADER_SEARCH              = "TÌM KIẾM";
inline const char* HEADER_SETTINGS            = "CÀI ĐẶT HỆ THỐNG ROMCLOUD";
inline const char* HEADER_WEB_CONNECT         = "LIÊN KẾT GOOGLE DRIVE QUA TRÌNH DUYỆT (LOCAL WEB)";
inline const char* HEADER_SYNC                = "ĐỒNG BỘ KHO GAME GOOGLE DRIVE";
inline const char* HEADER_DOWNLOAD            = "ĐANG TẢI ROM TỪ GOOGLE DRIVE";
inline const char* HEADER_DIAG                = "THÔNG TIN HỆ THỐNG & PHẦN CỨNG";
inline const char* HEADER_OTA                 = "CẬP NHẬT PHẦN MỀM (OTA UPDATE)";

// --- 2. MENU CHÍNH (MAIN MENU) ---
inline const char* MENU_PLAY                  = "KHO GAME (THƯ VIỆN)";
inline const char* MENU_SYNC                  = "ĐỒNG BỘ DỮ LIỆU";
inline const char* MENU_OTA                   = "CẬP NHẬT PHẦN MỀM (OTA)";
inline const char* MENU_OTA_NEW_BADGE         = "🚀 Cập nhật OTA [BẢN MỚI: v";
inline const char* MENU_SETTINGS              = "CÀI ĐẶT (SETTINGS)";
inline const char* MENU_DIAG                  = "THÔNG TIN HỆ THỐNG";
inline const char* MENU_EXIT                  = "THOÁT (EXIT)";

// --- 3. THANH ĐIỀU KHIỂN & NÚT BẤM (FOOTER CONTROLS) ---
inline const char* FOOTER_ENTER_SYSTEM        = "VÀO HỆ MÁY";
inline const char* FOOTER_MAIN_MENU           = "MENU CHÍNH";
inline const char* FOOTER_SYNC_DRIVE          = "ĐỒNG BỘ DRIVE";
inline const char* FOOTER_CHANGE_PAGE         = "CHUYỂN TRANG";
inline const char* FOOTER_ADD_QUEUE           = "+ HÀNG TẢI";
inline const char* FOOTER_BACK                = "QUAY LẠI";
inline const char* FOOTER_CANCEL              = "QUAY LẠI / HỦY";
inline const char* FOOTER_DELETE              = "BỎ HÀNG / XÓA";
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
inline const char* BTN_CANCEL                 = "QUAY LẠI / HỦY";
inline const char* BTN_DELETE_QUEUE           = "BỎ HÀNG / XÓA";
inline const char* BTN_DELETE_ROM_SD          = "XÓA ROM (THẺ)";
inline const char* BTN_DELETE_ROM             = "XÓA ROM";
inline const char* BTN_CONFIRM_DELETE_SD      = "XÁC NHẬN XÓA (THẺ)";
inline const char* BTN_CONFIRM_DELETE         = "[A] Xác nhận xóa";
inline const char* BTN_CANCEL_DELETE          = "[B] / [X] Hủy bỏ";
inline const char* BTN_SYNC                   = "ĐỒNG BỘ";
inline const char* BTN_FILTER                 = "BỘ LỌC";
inline const char* BTN_CONFIRM                = "XÁC NHẬN";
inline const char* BTN_SELECT                 = "CHỌN";
inline const char* BTN_JUMP_ALPHA             = "NHẢY CHỮ (A-Z)";
inline const char* BTN_SEARCH                 = "TÌM KIẾM";
inline const char* BTN_SELECT_CHAR            = "CHỌN KÝ TỰ";
inline const char* BTN_CLEAR_ALL              = "XÓA HẾT";
inline const char* BTN_DOWNLOAD               = "TẢI VỀ";
inline const char* BTN_CANCEL_ACTION          = "HỦY BỎ";
inline const char* BTN_BACK_MAIN_MENU_HINT    = "Nhấn [B] để quay lại Menu chính";

// --- 4. MÀN HÌNH CHỌN HỆ MÁY (SYSTEM SELECT) ---
inline const char* SYS_SELECT_TITLE           = "CHỌN HỆ MÁY";
inline const char* SYS_SELECT_SYSTEMS_LABEL   = " Hệ máy | ";
inline const char* SYS_SELECT_GAMES_LOCAL     = " Game trên thẻ | ";
inline const char* SYS_SELECT_GAMES_CLOUD     = " Game trên Cloud)";
inline const char* SYS_SELECT_SDCARD_PREFIX   = "THẺ NHỚ: ";
inline const char* SYS_SELECT_FOLDER_PREFIX   = "Thư mục: /Roms/";
inline const char* SYS_SELECT_EXT_PREFIX      = "  |  Định dạng: ";

// --- 5. MÀN HÌNH TÌM KIẾM (SEARCH SCREEN) ---
inline const char* SEARCH_PROMPT_MIN_CHARS    = "Nhập ít nhất 2 ký tự để tìm kiếm...";
inline const char* SEARCH_NO_RESULTS          = "Không tìm thấy kết quả.";
inline const char* SEARCH_RESULTS_SUFFIX      = " kết quả";
inline const char* SEARCH_NAV_UP_HINT         = "◀ Lên để quay lại bàn phím";
inline const char* SEARCH_NAV_DOWN_HINT       = "▼ Xuống để xem kết quả";

// --- 6. DANH SÁCH GAME & CHI TIẾT GAME (GAME LIST & DETAIL) ---
inline const char* GAME_LIST_EMPTY            = "Không có game nào phù hợp bộ lọc.";
inline const char* GAME_FILTER_HINT           = "Nhấn [SELECT] để chuyển bộ lọc (TẤT CẢ / THẺ NHỚ / CLOUD)";
inline const char* FILTER_TAG_ALL             = "[SELECT] LỌC: TẤT CẢ";
inline const char* FILTER_TAG_LOCAL           = "[SELECT] LỌC: THẺ NHỚ";
inline const char* FILTER_TAG_CLOUD           = "[SELECT] LỌC: CLOUD";
inline const char* FILTER_LABEL_ALL           = "Bộ lọc: TẤT CẢ GAME";
inline const char* FILTER_LABEL_LOCAL         = "Bộ lọc: CHỈ GAME TRÊN THẺ NHỚ";
inline const char* FILTER_LABEL_CLOUD         = "Bộ lọc: CHỈ GAME TRÊN CLOUD";
inline const char* BADGE_LOCAL                = "ĐÃ TẢI";
inline const char* BADGE_CLOUD                = "TRÊN CLOUD";
inline const char* BADGE_DOWNLOADED           = "✓ ĐÃ TẢI VỀ";
inline const char* BADGE_DOWNLOADING          = "ĐANG TẢI...";
inline const char* BADGE_DOWNLOADING_PCT      = "ĐANG TẢI... ";
inline const char* BADGE_QUEUED               = "CHỜ TẢI...";
inline const char* BADGE_DELETE_BTN           = "[X] XÓA ROM";
inline const char* BADGE_CANCEL_DL_BTN        = "[X] HỦY TẢI GAME NÀY";
inline const char* BADGE_REMOVE_QUEUE_BTN     = "[X] BỎ KHỎI HÀNG TẢI";
inline const char* BADGE_ADD_QUEUE_BTN        = "[A] THÊM VÀO HÀNG TẢI";
inline const char* DETAIL_SYSTEM              = "Hệ máy:";
inline const char* DETAIL_LOCATION            = "Vị trí:";
inline const char* DETAIL_SIZE                = "Dung lượng:";
inline const char* DETAIL_FILENAME            = "Tên tệp:";
inline const char* DETAIL_SYS_LABEL           = "Hệ máy:";
inline const char* DETAIL_SIZE_LABEL          = "Dung lượng:";
inline const char* DETAIL_LOCATION_LABEL      = "Vị trí lưu:";
inline const char* DETAIL_SD_PATH_PREFIX      = "Thẻ nhớ (/Roms/";
inline const char* DETAIL_LOCAL_STORAGE       = "Thẻ nhớ MicroSD";
inline const char* DETAIL_CLOUD_STORAGE       = "Google Drive Cloud";
inline const char* QUEUE_DOWNLOADING_ACTIVE   = "Đang tải 1 game, còn ";
inline const char* QUEUE_REMAINING_SUFFIX     = " game chờ.";
inline const char* QUEUE_DOWNLOADING_EMPTY    = "Đang tải... Hàng tải trống.";
inline const char* QUEUE_WAITING_PREFIX       = "Hàng tải: ";

// --- 7. HỘP THOẠI XÓA ROM (DELETE CONFIRMATION DIALOG) ---
inline const char* DELETE_TITLE               = "XÓA BẢN SAO TRÊN THẺ NHỚ?";
inline const char* DELETE_DESC                = "Thao tác này chỉ xóa file ROM khỏi thẻ SD để giải phóng bộ nhớ.";
inline const char* DELETE_CLOUD_SAFE          = "Bản lưu trên Google Drive vẫn an toàn và có thể tải lại bất cứ lúc nào.";
inline const char* DELETE_CONFIRM_BTN         = "[A] Xác nhận xóa";
inline const char* DELETE_CANCEL_BTN          = "[B] Hủy bỏ";
inline const char* DIALOG_DELETE_TITLE        = "XÁC NHẬN XÓA ROM TRÊN THẺ NHỚ";
inline const char* DIALOG_DELETE_FILE_LABEL   = "Tập tin: ";
inline const char* DIALOG_DELETE_PROMPT       = "Bạn có chắc chắn muốn xóa ROM này khỏi thẻ nhớ không?";
inline const char* DIALOG_DELETE_SAFE_HINT    = "Bản lưu trên Google Drive vẫn an toàn và có thể tải lại bất cứ lúc nào.";

// --- 8. BẢNG TUYÊN BỐ MIỄN TRỪ TRÁCH NHIỆM (DISCLAIMER) ---
inline const char* DISCLAIMER_TITLE           = "TUYÊN BỐ MIỄN TRỪ TRÁCH NHIỆM BẢN QUYỀN";
inline const char* DISCLAIMER_SUBTITLE        = "XÁC NHẬN BẢN QUYỀN TRƯỚC KHI KẾT NỐI GOOGLE DRIVE";
inline const char* DISCLAIMER_SEC1_TITLE      = "1. PHẠM VI ỨNG DỤNG:";
inline const char* DISCLAIMER_SEC1_LINE1      = "RomCloud là phần mềm tiện ích mã nguồn mở phục vụ đồng bộ dữ liệu cá nhân.";
inline const char* DISCLAIMER_SEC1_LINE2      = "Ứng dụng KHÔNG cung cấp, KHÔNG lưu trữ và KHÔNG phân phối bất kỳ tệp ROM nào.";
inline const char* DISCLAIMER_SEC2_TITLE      = "2. NGUỒN TẬP TIN & DỮ LIỆU:";
inline const char* DISCLAIMER_SEC2_LINE1      = "Tất cả danh sách game, tập tin ROM và nội dung tải về máy TrimUI đều được lấy trực tiếp";
inline const char* DISCLAIMER_SEC2_LINE2      = "từ tài khoản Google Drive do chính bạn sở hữu và cung cấp quyền truy cập.";
inline const char* DISCLAIMER_SEC3_TITLE      = "3. TRÁCH NHIỆM PHÁP LÝ CỦA NGƯỜI DÙNG:";
inline const char* DISCLAIMER_SEC3_LINE1      = "Bạn cam kết chỉ sử dụng các bản sao lưu ROM hợp pháp thuộc quyền sở hữu của bạn.";
inline const char* DISCLAIMER_SEC3_LINE2      = "Người dùng hoàn toàn tự chịu mọi trách nhiệm trước pháp luật về vấn đề bản quyền,";
inline const char* DISCLAIMER_SEC3_LINE3      = "quyền tác giả đối với toàn bộ các tệp tin lưu trữ và tải về từ Drive của mình.";
inline const char* DISCLAIMER_AGREE           = "[A] Tôi đồng ý & Tiếp tục";
inline const char* DISCLAIMER_DECLINE         = "[B] Từ chối & Quay lại";

// --- 9. CÀI ĐẶT HỆ THỐNG (SETTINGS) ---
inline const char* SETTING_DRIVE_STATUS       = "Trạng thái Google Drive:";
inline const char* SETTING_CONNECTED          = "ĐÃ KẾT NỐI";
inline const char* SETTING_DISCONNECTED       = "CHƯA KẾT NỐI";
inline const char* SETTING_LOGOUT_BTN         = "[X] Đăng xuất";
inline const char* SETTING_CONNECT_WEB_BTN    = "[A] Kết nối qua Web Local";
inline const char* SETTING_DRIVE_FOLDER       = "Thư mục Google Drive:";
inline const char* SETTING_NOT_CONFIGURED     = "Chưa thiết lập";
inline const char* SETTING_ROM_SD_FOLDER      = "Thư mục ROM trên thẻ:";
inline const char* SETTING_LAST_SYNC          = "Đồng bộ lần cuối:";
inline const char* SETTING_NEVER_SYNCED       = "Chưa đồng bộ";
inline const char* SETTING_SQLITE_DB          = "Cơ sở dữ liệu SQLite:";
inline const char* SETTING_SCAN_MODE          = "Chế độ quét Drive:";
inline const char* SETTING_SCAN_AUTO          = "Tự động (Quét toàn bộ danh mục game)";
inline const char* SETTING_WEB_PORTAL         = "Trang quản lý nội bộ:";
inline const char* SETTING_COVER_CACHE        = "Bộ đệm ảnh bìa (Cover):";
inline const char* SETTING_COVER_CACHE_VAL    = "Phần cứng SDL2_image (Tối đa 64 ảnh)";

// --- 10. HƯỚNG DẪN KẾT NỐI QUA WEB (WEB CONNECTION SCREEN) ---
inline const char* WEB_CONNECT_TITLE          = "LIÊN KẾT GOOGLE DRIVE QUA TRÌNH DUYỆT (LOCAL WEB)";
inline const char* WEB_CONNECT_GUIDE_TITLE    = "HƯỚNG DẪN KẾT NỐI TÀI KHOẢN QUA TRÌNH DUYỆT";
inline const char* WEB_CONNECT_STEP1          = "BƯỚC 1: Kết nối điện thoại hoặc máy tính vào cùng mạng Wi-Fi với TrimUI.";
inline const char* WEB_CONNECT_STEP2          = "BƯỚC 2: Mở trình duyệt web (Chrome, Safari, Cốc Cốc...) và truy cập địa chỉ:";
inline const char* WEB_CONNECT_STEP3          = "BƯỚC 3: Trên trang web, bấm \"ĐĂNG NHẬP GOOGLE\" hoặc nhập liên kết thư mục Drive.";
inline const char* WEB_CONNECT_WAITING        = "* Đang chờ kết nối từ trình duyệt web của bạn...";
inline const char* WEB_CONNECT_AUTO_HINT      = "Máy sẽ tự động hoàn tất ngay khi bạn đăng nhập thành công trên điện thoại/PC.";
inline const char* WEB_CONNECT_BACK_BTN       = "[B] Quay lại Cài đặt";
inline const char* WEB_CONNECT_SUCCESS        = "ĐÃ KẾT NỐI THÀNH CÔNG!";
inline const char* WEB_CONNECT_ACCOUNT_PREFIX = "Tài khoản Google đã liên kết:";
inline const char* WEB_CONNECT_READY          = "Toàn bộ kho game đã sẵn sàng để tải về máy và chơi.";
inline const char* WEB_CONNECT_START_BTN      = "[A] / [B] Bắt đầu sử dụng";

// --- 11. ĐỒNG BỘ GOOGLE DRIVE (SYNC SCREEN) ---
inline const char* SYNC_TITLE                 = "ĐỒNG BỘ DỮ LIỆU CLOUD";
inline const char* SYNC_CONNECTING            = "Đang kết nối tới Google Drive API...";
inline const char* SYNC_SCANNING              = "Đang quét danh mục trò chơi trên Google Drive...";
inline const char* SYNC_CANCEL_HINT           = "[B] Hủy đồng bộ";
inline const char* SYNC_API_CONNECTING        = "Đang kết nối tới Google Drive API v3...";
inline const char* SYNC_AUTH_TOKEN            = "Xác thực mã thông báo OAuth Bearer...";
inline const char* SYNC_SCANNING_GAMES        = "Đang quét danh mục trò chơi trên Google Drive...";
inline const char* SYNC_SEARCH_FOLDERS        = "Tìm kiếm thư mục /RomCloud hoặc /Roms...";
inline const char* SYNC_SCANNING_SYS_PREFIX   = "Đang quét hệ máy: ";
inline const char* SYNC_SYS_PREFIX            = "Hệ máy ";
inline const char* SYNC_SAVED_PREFIX          = "Đã lưu vào kho: ";
inline const char* SYNC_NEW_PREFIX            = "  (Mới: ";
inline const char* SYNC_UPDATED_PREFIX        = ", Cập nhật: ";
inline const char* SYNC_CANCEL_BTN            = "[B] Hủy đồng bộ";

// --- 12. TIẾN TRÌNH TẢI ROM (DOWNLOAD SCREEN) ---
inline const char* DL_SYS_PREFIX              = "Hệ máy: ";
inline const char* DL_FILE_PREFIX             = "  |  Tập tin: ";
inline const char* DL_CONNECTING              = "Đang kết nối...";
inline const char* DL_CHECKING_FILE           = "Đang kiểm tra tập tin...";
inline const char* DL_FROM_DRIVE              = "Đang tải từ Google Drive...";
inline const char* DL_REMAINING_QUEUE_PREFIX  = "Còn ";
inline const char* DL_REMAINING_QUEUE_SUFFIX  = " game trong hàng chờ.";
inline const char* DL_CANCEL_BTN              = "[B] Hủy tải về";

// --- 13. THÔNG TIN PHẦN CỨNG & HỆ THỐNG (DIAGNOSTICS SCREEN) ---
inline const char* DIAG_HW_DEVICE             = "Thiết bị phần cứng";
inline const char* DIAG_CPU_ARCH              = "Kiến trúc CPU";
inline const char* DIAG_OS_KERNEL             = "Hệ điều hành & Nhân";
inline const char* DIAG_RAM                   = "Bộ nhớ RAM";
inline const char* DIAG_FREE_PREFIX           = "Còn trống ";
inline const char* DIAG_TOTAL_SEPARATOR       = " / Tổng ";
inline const char* DIAG_DISPLAY               = "Màn hình hiển thị";
inline const char* DIAG_SDL2_GFX              = "Thư viện đồ họa SDL2";
inline const char* DIAG_HW_ACCEL              = " (Tăng tốc phần cứng)";
inline const char* DIAG_SQLITE_DB             = "Cơ sở dữ liệu SQLite3";
inline const char* DIAG_SCHEMA_VER_PREFIX     = " (Phiên bản cấu trúc v";
inline const char* DIAG_SD_STORAGE            = "Dung lượng thẻ nhớ SD";
inline const char* DIAG_GAMEPAD               = "Cụm phím điều khiển";
inline const char* DIAG_WIFI                  = "Kết nối mạng Wi-Fi";
inline const char* DIAG_SAFETY                = "Độ an toàn hệ thống";
inline const char* DIAG_SAFETY_VAL            = "100% Lưu trên thẻ nhớ (Không sửa đổi /rom, /usr, /overlay)";

// --- 14. CẬP NHẬT PHẦN MỀM OTA (OTA UPDATE SCREEN) ---
inline const char* OTA_TITLE                  = "CẬP NHẬT HỆ THỐNG (OTA)";
inline const char* OTA_CHECKING               = "Đang kiểm tra cập nhật từ GitHub...";
inline const char* OTA_UP_TO_DATE             = "Phiên bản hiện tại đã là mới nhất!";
inline const char* OTA_NEW_VERSION            = "Đã có phiên bản mới!";
inline const char* OTA_INSTALLING             = "Đang tải và cập nhật RomCloud...";
inline const char* OTA_SUCCESS                = "Cập nhật thành công! Nhấn [A] để khởi động lại.";
inline const char* OTA_FAILED                 = "Cập nhật thất bại. Vui lòng kiểm tra kết nối Wi-Fi.";
inline const char* OTA_CURRENT_VER_PREFIX     = "Phiên bản hiện tại trên máy: v";
inline const char* OTA_SOURCE_PREFIX          = "Nguồn phát hành: GitHub @";
inline const char* OTA_WAITING                = "Vui lòng đợi trong giây lát...";
inline const char* OTA_STATUS_UP_TO_DATE      = "ĐÃ MỚI NHẤT";
inline const char* OTA_MSG_UP_TO_DATE         = "Bạn đang sử dụng phiên bản mới nhất!";
inline const char* OTA_NO_NEW_UPDATE          = "Không có bản cập nhật mới nào trên GitHub repository.";
inline const char* OTA_BTNS_CHECK_BACK        = "[A] Kiểm tra lại   |   [B] Quay lại";
inline const char* OTA_STATUS_NEW_UPDATE      = "CÓ BẢN CẬP NHẬT MỚI";
inline const char* OTA_NEW_VER_PREFIX         = "Phiên bản mới: v";
inline const char* OTA_CHANGELOG_TITLE        = "Nội dung cập nhật:";
inline const char* OTA_BTN_INSTALL_NOW        = "[A] TẢI VỀ VÀ CẬP NHẬT NGAY";
inline const char* OTA_DOWNLOADING_TITLE      = "ĐANG TẢI BẢN CẬP NHẬT TỪ GITHUB...";
inline const char* OTA_VERIFYING_FILE         = "Đang kiểm tra tính toàn vẹn tập tin nhị phân...";
inline const char* OTA_BTN_CANCEL_DOWNLOAD    = "[B] Hủy cập nhật";
inline const char* OTA_STATUS_COMPLETED       = "HOÀN TẤT!";
inline const char* OTA_MSG_COMPLETED          = "Cập nhật hoàn tất thành công!";
inline const char* OTA_MSG_RESTART_HINT       = "Bản cập nhật sẽ có hiệu lực ngay sau khi khởi động lại.";
inline const char* OTA_BTN_RESTART_NOW        = "[A] KHỞI ĐỘNG LẠI NGAY (RESTART)";
inline const char* OTA_STATUS_FAILED          = "THẤT BÀI";
inline const char* OTA_MSG_FAILED             = "Không thể cập nhật phần mềm!";
inline const char* OTA_ERR_NETWORK            = "Lỗi kết nối mạng hoặc GitHub.";
inline const char* OTA_BTNS_RETRY_BACK        = "[A] Thử lại   |   [B] Quay lại";

// --- 15. THÔNG BÁO TOAST & TRẠNG THÁI (TOAST NOTIFICATIONS) ---
inline const char* TOAST_NEW_OTA_PREFIX       = "Đã có bản cập nhật mới v";
inline const char* TOAST_SYNCING_DRIVE        = "Đang đồng bộ dữ liệu với Google Drive...";
inline const char* TOAST_SCANNING_SD          = "Đang quét thẻ nhớ...";
inline const char* TOAST_SD_SCANNED_NO_DRIVE  = "Đã quét xong thẻ nhớ (Chưa liên kết Drive).";
inline const char* TOAST_DOWNLOAD_COMPLETE    = "Đã tải xong: ";
inline const char* TOAST_UNKNOWN_ERROR        = "Lỗi không xác định.";
inline const char* TOAST_DOWNLOAD_FAILED      = "Tải thất bại: ";
inline const char* TOAST_SYNC_CANCELLED       = "Đã hủy đồng bộ Cloud.";
inline const char* TOAST_SYNC_NO_DRIVE        = "Chưa liên kết thư mục Google Drive.";
inline const char* TOAST_SYNC_COMPLETED_PRE   = "Đồng bộ hoàn tất: Đã lưu ";
inline const char* TOAST_SYNC_COMPLETED_SUF   = " game vào thư viện!";
inline const char* TOAST_GOOGLE_LOGIN_SUCCESS = "Đăng nhập Google thành công! Bấm [Y] để đồng bộ.";
inline const char* TOAST_GAME_EXISTS_DELETE   = "Game đã có trên thẻ nhớ. Nhấn [X] để XÓA ROM khỏi thẻ.";
inline const char* TOAST_ALREADY_IN_QUEUE     = " đã có trong danh sách tải.";
inline const char* TOAST_ADDED_TO_QUEUE       = "Đã thêm vào hàng tải: ";
inline const char* TOAST_CONNECT_DRIVE_FIRST  = "Kết nối Google Drive trước!";
inline const char* TOAST_DOWNLOAD_STOPPED     = "Đã dừng tải: ";
inline const char* TOAST_REMOVED_FROM_QUEUE   = "Đã bỏ khỏi hàng tải: ";
inline const char* TOAST_GAME_ONLY_ON_DRIVE   = "Game này chưa tải về thẻ nhớ (chỉ có trên Drive).";
inline const char* TOAST_JUMP_ALPHA_PRE       = "Chuyển đến vần chữ: [ ";
inline const char* TOAST_DELETED_PRE          = "Đã xóa \"";
inline const char* TOAST_DELETED_SUF          = "\" khỏi thẻ nhớ.";
inline const char* TOAST_UNLINK_SUCCESS       = "Đã hủy liên kết Google Drive.";
inline const char* TOAST_LOGOUT_SUCCESS       = "Đã đăng xuất khỏi Google Drive.";
inline const char* TOAST_OTA_CANCELLED        = "Đã hủy cập nhật phần mềm.";

} // namespace UiStrings
} // namespace RomCloud
