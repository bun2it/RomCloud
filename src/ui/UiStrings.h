#pragma once

#include <string>

namespace RomCloud {
namespace UiStrings {

// ============================================================================
// CẤU HÌNH CÂU CHỮ GIAO DIỆN MÁY TRIMUI BRICK PRO
// Bạn có thể chỉnh sửa trực tiếp các chuỗi văn bản này và compile vào app!
// ============================================================================

// --- TIÊU ĐỀ & HEADER ---
inline const char* APP_TITLE              = "ROMCLOUD";
inline const char* APP_SUBTITLE           = "TRIMUI BRICK PRO";
inline const char* SYSTEM_SELECT_TITLE    = "CHỌN HỆ MÁY";

// --- THANH ĐIỀU KHIỂN (FOOTER NÚT BẤM) ---
inline const char* FOOTER_ENTER_SYSTEM    = "VÀO HỆ MÁY";
inline const char* FOOTER_MAIN_MENU       = "MENU CHÍNH";
inline const char* FOOTER_SYNC_DRIVE      = "ĐỒNG BỘ DRIVE";
inline const char* FOOTER_CHANGE_PAGE     = "CHUYỂN TRANG";
inline const char* FOOTER_ADD_QUEUE       = "+ HÀNG TẢI";
inline const char* FOOTER_BACK            = "QUAY LẠI";
inline const char* FOOTER_CANCEL          = "QUAY LẠI / HỦY";
inline const char* FOOTER_DELETE          = "BỎ HÀNG / XÓA";
inline const char* FOOTER_SYNC            = "ĐỒNG BỘ";
inline const char* FOOTER_FILTER          = "BỘ LỌC";
inline const char* FOOTER_CONFIRM         = "XÁC NHẬN";
inline const char* FOOTER_SELECT          = "CHỌN";

// --- MENU CHÍNH (MAIN MENU) ---
inline const char* MENU_PLAY              = "KHO GAME (THƯ VIỆN)";
inline const char* MENU_SYNC              = "ĐỒNG BỘ DỮ LIỆU";
inline const char* MENU_OTA               = "CẬP NHẬT PHẦN MỀM (OTA)";
inline const char* MENU_SETTINGS          = "CÀI ĐẶT (SETTINGS)";
inline const char* MENU_DIAG              = "THÔNG TIN HỆ THỐNG";
inline const char* MENU_EXIT              = "THOÁT (EXIT)";

// --- BẢNG TUYÊN BỐ MIỄN TRỪ TRÁCH NHIỆM (DISCLAIMER) ---
inline const char* DISCLAIMER_TITLE       = "TUYÊN BỐ MIỄN TRỪ TRÁCH NHIỆM BẢN QUYỀN";
inline const char* DISCLAIMER_SUBTITLE    = "XÁC NHẬN BẢN QUYỀN TRƯỚC KHI KẾT NỐI GOOGLE DRIVE";
inline const char* DISCLAIMER_SEC1_TITLE  = "1. PHẠM VI ỨNG DỤNG:";
inline const char* DISCLAIMER_SEC1_LINE1  = "RomCloud là phần mềm tiện ích mã nguồn mở phục vụ đồng bộ dữ liệu cá nhân.";
inline const char* DISCLAIMER_SEC1_LINE2  = "Ứng dụng KHÔNG cung cấp, KHÔNG lưu trữ và KHÔNG phân phối bất kỳ tệp ROM nào.";
inline const char* DISCLAIMER_SEC2_TITLE  = "2. NGUỒN TẬP TIN & DỮ LIỆU:";
inline const char* DISCLAIMER_SEC2_LINE1  = "Tất cả danh sách game, tập tin ROM và nội dung tải về máy TrimUI đều được lấy trực tiếp";
inline const char* DISCLAIMER_SEC2_LINE2  = "từ tài khoản Google Drive do chính bạn sở hữu và cung cấp quyền truy cập.";
inline const char* DISCLAIMER_SEC3_TITLE  = "3. TRÁCH NHIỆM PHÁP LÝ CỦA NGƯỜI DÙNG:";
inline const char* DISCLAIMER_SEC3_LINE1  = "Bạn cam kết chỉ sử dụng các bản sao lưu ROM hợp pháp thuộc quyền sở hữu của bạn.";
inline const char* DISCLAIMER_SEC3_LINE2  = "Người dùng hoàn toàn tự chịu mọi trách nhiệm trước pháp luật về vấn đề bản quyền,";
inline const char* DISCLAIMER_SEC3_LINE3  = "quyền tác giả đối với toàn bộ các tệp tin lưu trữ và tải về từ Drive của mình.";
inline const char* DISCLAIMER_AGREE       = "[A] Tôi đồng ý & Tiếp tục";
inline const char* DISCLAIMER_DECLINE     = "[B] Từ chối & Quay lại";

// --- DANH SÁCH GAME & CHI TIẾT GAME ---
inline const char* GAME_LIST_EMPTY        = "Không có game nào phù hợp bộ lọc.";
inline const char* GAME_FILTER_HINT       = "Nhấn [SELECT] để chuyển bộ lọc (TẤT CẢ / THẺ NHỚ / CLOUD)";
inline const char* BADGE_LOCAL            = "ĐÃ TẢI";
inline const char* BADGE_CLOUD            = "TRÊN CLOUD";
inline const char* BADGE_DOWNLOADING      = "ĐANG TẢI...";
inline const char* BADGE_QUEUED           = "CHỜ TẢI...";
inline const char* DETAIL_SYSTEM          = "Hệ máy:";
inline const char* DETAIL_LOCATION        = "Vị trí:";
inline const char* DETAIL_SIZE            = "Dung lượng:";
inline const char* DETAIL_FILENAME        = "Tên tệp:";
inline const char* DETAIL_LOCAL_STORAGE   = "Thẻ nhớ MicroSD";
inline const char* DETAIL_CLOUD_STORAGE   = "Google Drive Cloud";

// --- HỘP THOẠI XÓA / BỎ TẢI ---
inline const char* DELETE_TITLE           = "XÓA BẢN SAO TRÊN THẺ NHỚ?";
inline const char* DELETE_DESC            = "Thao tác này chỉ xóa file ROM khỏi thẻ SD để giải phóng bộ nhớ.";
inline const char* DELETE_CLOUD_SAFE      = "Bản lưu trên Google Drive vẫn an toàn và có thể tải lại bất cứ lúc nào.";
inline const char* DELETE_CONFIRM_BTN     = "[A] Xác nhận xóa";
inline const char* DELETE_CANCEL_BTN      = "[B] Hủy bỏ";

// --- TIẾN TRÌNH ĐỒNG BỘ (SYNC) ---
inline const char* SYNC_TITLE             = "ĐỒNG BỘ DỮ LIỆU CLOUD";
inline const char* SYNC_CONNECTING        = "Đang kết nối tới Google Drive API...";
inline const char* SYNC_SCANNING          = "Đang quét danh mục trò chơi trên Google Drive...";
inline const char* SYNC_CANCEL_HINT       = "[B] Hủy đồng bộ";

// --- KẾT NỐI WEB TRÊN MÁY TRIMUI ---
inline const char* WEB_CONNECT_TITLE      = "LIÊN KẾT GOOGLE DRIVE QUA TRÌNH DUYỆT (LOCAL WEB)";
inline const char* WEB_CONNECT_SUCCESS    = "ĐÃ KẾT NỐI THÀNH CÔNG!";
inline const char* WEB_CONNECT_READY      = "Toàn bộ kho game đã sẵn sàng để tải về máy và chơi.";
inline const char* WEB_CONNECT_START_BTN  = "[A] / [B] Bắt đầu sử dụng";
inline const char* WEB_CONNECT_STEP1      = "1. Kết nối điện thoại hoặc máy tính vào cùng mạng Wi-Fi với máy TrimUI.";
inline const char* WEB_CONNECT_STEP2      = "2. Mở trình duyệt web và truy cập vào địa chỉ IP:";
inline const char* WEB_CONNECT_STEP3      = "3. Dán liên kết thư mục Google Drive để tự động quét toàn bộ game!";

// --- CẬP NHẬT HỆ THỐNG (OTA) ---
inline const char* OTA_TITLE              = "CẬP NHẬT HỆ THỐNG (OTA)";
inline const char* OTA_CHECKING           = "Đang kiểm tra cập nhật từ GitHub...";
inline const char* OTA_UP_TO_DATE         = "Phiên bản hiện tại đã là mới nhất!";
inline const char* OTA_NEW_VERSION        = "Đã có phiên bản mới!";
inline const char* OTA_INSTALLING         = "Đang tải và cập nhật RomCloud...";
inline const char* OTA_SUCCESS            = "Cập nhật thành công! Nhấn [A] để khởi động lại.";
inline const char* OTA_FAILED             = "Cập nhật thất bại. Vui lòng kiểm tra kết nối Wi-Fi.";

// --- THÔNG BÁO TOAST ---
inline const char* TOAST_SYNC_CANCELLED   = "Đã hủy đồng bộ Cloud.";
inline const char* TOAST_SYNC_NO_DRIVE    = "Chưa liên kết thư mục Google Drive.";
inline const char* TOAST_UNLINK_SUCCESS   = "Đã hủy liên kết Google Drive.";

} // namespace UiStrings
} // namespace RomCloud
