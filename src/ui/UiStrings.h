#pragma once

#include <string>

namespace RomCloud {
namespace UiStrings {

// ============================================================================
// CẤU HÌNH GIAO DIỆN — ROMCLOUD CHO TRIMUI BRICK PRO
// Chỉnh sửa chuỗi văn bản tại đây và biên dịch lại.
// ============================================================================

// --- 1. TIÊU ĐỀ CHUNG & HEADER ---
extern const char *APP_TITLE;
extern const char *APP_SUBTITLE;
extern const char *SYSTEM_SELECT_TITLE;
extern const char *HEADER_SYSTEM_SELECT;
extern const char *HEADER_SEARCH;
extern const char *HEADER_SETTINGS;
extern const char *HEADER_WEB_CONNECT;
extern const char *HEADER_SYNC;
extern const char *HEADER_DOWNLOAD;
extern const char *HEADER_DIAG;
extern const char *HEADER_OTA;

// --- 2. MENU CHÍNH (MAIN MENU) ---
extern const char *MENU_PLAY;
extern const char *MENU_IPTV;
extern const char *MENU_SYNC;
extern const char *MENU_REVERSE_SYNC;
extern const char *MENU_OTA;
extern const char *MENU_OTA_NEW_BADGE;
extern const char *MENU_SETTINGS;
extern const char *MENU_DIAG;
extern const char *MENU_EXIT;

// --- IPTV STRINGS ---
extern const char *IPTV_TITLE;
extern const char *IPTV_NO_CHANNELS;
extern const char *IPTV_LOADING;
extern const char *IPTV_NOW_PLAYING;

// --- 3. THANH ĐIỀU KHIỂN & NÚT BẤM ---
extern const char *FOOTER_ENTER_SYSTEM;
extern const char *FOOTER_MAIN_MENU;
extern const char *FOOTER_SYNC_DRIVE;
extern const char *FOOTER_CHANGE_PAGE;
extern const char *FOOTER_ADD_QUEUE;
extern const char *FOOTER_BACK;
extern const char *FOOTER_CANCEL;
extern const char *FOOTER_DELETE;
extern const char *FOOTER_SYNC;
extern const char *FOOTER_FILTER;
extern const char *FOOTER_CONFIRM;
extern const char *FOOTER_SELECT;

extern const char *BTN_ENTER_SYSTEM;
extern const char *BTN_MAIN_MENU;
extern const char *BTN_SYNC_DRIVE;
extern const char *BTN_CHANGE_PAGE;
extern const char *BTN_ADD_QUEUE;
extern const char *BTN_BACK;
extern const char *BTN_CANCEL;
extern const char *BTN_DELETE_QUEUE;
extern const char *BTN_DELETE_ROM_SD;
extern const char *BTN_DELETE_ROM;
extern const char *BTN_CONFIRM_DELETE_SD;
extern const char *BTN_CONFIRM_DELETE;
extern const char *BTN_CANCEL_DELETE;
extern const char *BTN_SYNC;
extern const char *BTN_FILTER;
extern const char *BTN_CONFIRM;
extern const char *BTN_SELECT;
extern const char *BTN_JUMP_ALPHA;
extern const char *BTN_SEARCH;
extern const char *BTN_SELECT_CHAR;
extern const char *BTN_CLEAR_ALL;
extern const char *BTN_DOWNLOAD;
extern const char *BTN_CANCEL_ACTION;
extern const char *BTN_BACK_MAIN_MENU_HINT;

// --- 4. MÀN HÌNH CHỌN HỆ MÁY ---
extern const char *SYS_SELECT_TITLE;
extern const char *SYS_SELECT_SYSTEMS_LABEL;
extern const char *SYS_SELECT_GAMES_LOCAL;
extern const char *SYS_SELECT_GAMES_CLOUD;
extern const char *SYS_SELECT_SDCARD_PREFIX;
extern const char *SYS_SELECT_FOLDER_PREFIX;
extern const char *SYS_SELECT_EXT_PREFIX;

// --- 5. MÀN HÌNH TÌM KIẾM ---
extern const char *SEARCH_PROMPT_MIN_CHARS;
extern const char *SEARCH_NO_RESULTS;
extern const char *SEARCH_RESULTS_SUFFIX;
extern const char *SEARCH_NAV_UP_HINT;
extern const char *SEARCH_NAV_DOWN_HINT;

// --- 6. DANH SÁCH GAME & CHI TIẾT GAME ---
extern const char *GAME_LIST_EMPTY;
extern const char *GAME_FILTER_HINT;
extern const char *FILTER_TAG_ALL;
extern const char *FILTER_TAG_LOCAL;
extern const char *FILTER_TAG_CLOUD;
extern const char *FILTER_LABEL_ALL;
extern const char *FILTER_LABEL_LOCAL;
extern const char *FILTER_LABEL_CLOUD;
extern const char *BADGE_LOCAL;
extern const char *BADGE_CLOUD;
extern const char *BADGE_DOWNLOADED;
extern const char *BADGE_DOWNLOADING;
extern const char *BADGE_DOWNLOADING_PCT;
extern const char *BADGE_QUEUED;
extern const char *BADGE_DELETE_BTN;
extern const char *BADGE_CANCEL_DL_BTN;
extern const char *BADGE_REMOVE_QUEUE_BTN;
extern const char *BADGE_ADD_QUEUE_BTN;
extern const char *DETAIL_SYSTEM;
extern const char *DETAIL_LOCATION;
extern const char *DETAIL_SIZE;
extern const char *DETAIL_FILENAME;
extern const char *DETAIL_SYS_LABEL;
extern const char *DETAIL_SIZE_LABEL;
extern const char *DETAIL_LOCATION_LABEL;
extern const char *DETAIL_SD_PATH_PREFIX;
extern const char *DETAIL_LOCAL_STORAGE;
extern const char *DETAIL_CLOUD_STORAGE;
extern const char *QUEUE_DOWNLOADING_ACTIVE;
extern const char *QUEUE_REMAINING_SUFFIX;
extern const char *QUEUE_DOWNLOADING_EMPTY;
extern const char *QUEUE_WAITING_PREFIX;

// --- 7. HỘP THOẠI XÓA ROM ---
extern const char *DELETE_TITLE;
extern const char *DELETE_DESC;
extern const char *DELETE_CLOUD_SAFE;
extern const char *DELETE_CONFIRM_BTN;
extern const char *DELETE_CANCEL_BTN;
extern const char *DIALOG_DELETE_TITLE;
extern const char *DIALOG_DELETE_FILE_LABEL;
extern const char *DIALOG_DELETE_PROMPT;
extern const char *DIALOG_DELETE_SAFE_HINT;

// --- 8. TUYÊN BỐ BẢN QUYỀN ---
extern const char *DISCLAIMER_TITLE;
extern const char *DISCLAIMER_SUBTITLE;
extern const char *DISCLAIMER_SEC1_TITLE;
extern const char *DISCLAIMER_SEC1_LINE1;
extern const char *DISCLAIMER_SEC1_LINE2;
extern const char *DISCLAIMER_SEC2_TITLE;
extern const char *DISCLAIMER_SEC2_LINE1;
extern const char *DISCLAIMER_SEC2_LINE2;
extern const char *DISCLAIMER_SEC3_TITLE;
extern const char *DISCLAIMER_SEC3_LINE1;
extern const char *DISCLAIMER_SEC3_LINE2;
extern const char *DISCLAIMER_SEC3_LINE3;
extern const char *DISCLAIMER_AGREE;
extern const char *DISCLAIMER_DECLINE;

// --- 9. CÀI ĐẶT HỆ THỐNG ---
extern const char *SETTING_DRIVE_STATUS;
extern const char *SETTING_CONNECTED;
extern const char *SETTING_DISCONNECTED;
extern const char *SETTING_LOGOUT_BTN;
extern const char *SETTING_CONNECT_WEB_BTN;
extern const char *SETTING_DRIVE_FOLDER;
extern const char *SETTING_NOT_CONFIGURED;
extern const char *SETTING_ROM_SD_FOLDER;
extern const char *SETTING_LAST_SYNC;
extern const char *SETTING_NEVER_SYNCED;
extern const char *SETTING_SQLITE_DB;
extern const char *SETTING_SCAN_MODE;
extern const char *SETTING_SCAN_AUTO;
extern const char *SETTING_WEB_PORTAL;
extern const char *SETTING_COVER_CACHE;
extern const char *SETTING_COVER_CACHE_VAL;

// --- 10. KẾT NỐI QUA WEB ---
extern const char *WEB_CONNECT_TITLE;
extern const char *WEB_CONNECT_GUIDE_TITLE;
extern const char *WEB_CONNECT_STEP1;
extern const char *WEB_CONNECT_STEP2;
extern const char *WEB_CONNECT_STEP3;
extern const char *WEB_CONNECT_WAITING;
extern const char *WEB_CONNECT_AUTO_HINT;
extern const char *WEB_CONNECT_BACK_BTN;
extern const char *WEB_CONNECT_SUCCESS;
extern const char *WEB_CONNECT_ACCOUNT_PREFIX;
extern const char *WEB_CONNECT_READY;
extern const char *WEB_CONNECT_START_BTN;

// --- 11. ĐỒNG BỘ GOOGLE DRIVE ---
extern const char *SYNC_TITLE;
extern const char *SYNC_CONNECTING;
extern const char *SYNC_SCANNING;
extern const char *SYNC_CANCEL_HINT;
extern const char *SYNC_API_CONNECTING;
extern const char *SYNC_AUTH_TOKEN;
extern const char *SYNC_SCANNING_GAMES;
extern const char *SYNC_SEARCH_FOLDERS;
extern const char *SYNC_SCANNING_SYS_PREFIX;
extern const char *SYNC_SYS_PREFIX;
extern const char *SYNC_SAVED_PREFIX;
extern const char *SYNC_NEW_PREFIX;
extern const char *SYNC_UPDATED_PREFIX;
extern const char *SYNC_CANCEL_BTN;

// --- 11B. REVERSE SYNC (UPLOAD) ---
extern const char *REVERSE_SYNC_TITLE;
extern const char *REVERSE_SYNC_PREPARING;
extern const char *REVERSE_SYNC_UPLOADING;
extern const char *REVERSE_SYNC_GAME_PROGRESS;
extern const char *REVERSE_SYNC_STATS;
extern const char *REVERSE_SYNC_SUCCESS;
extern const char *REVERSE_SYNC_SUCCESS_SUF;
extern const char *REVERSE_SYNC_FAILED;
extern const char *REVERSE_SYNC_CANCELLED;
extern const char *REVERSE_SYNC_NO_GAMES;
extern const char *REVERSE_SYNC_NOT_LINKED;
extern const char *REVERSE_SYNC_BTN;
extern const char *REVERSE_SYNC_CANCEL_BTN;
extern const char *REVERSE_SYNC_GAMES_FOUND;
extern const char *REVERSE_SYNC_GAMES_SUF;

// --- 12. TIẾN TRÌNH TẢI ROM ---
extern const char *DL_SYS_PREFIX;
extern const char *DL_FILE_PREFIX;
extern const char *DL_CONNECTING;
extern const char *DL_CHECKING_FILE;
extern const char *DL_FROM_DRIVE;
extern const char *DL_REMAINING_QUEUE_PREFIX;
extern const char *DL_REMAINING_QUEUE_SUFFIX;
extern const char *DL_CANCEL_BTN;

// --- 13. THÔNG TIN HỆ THỐNG ---
extern const char *DIAG_HW_DEVICE;
extern const char *DIAG_CPU_ARCH;
extern const char *DIAG_OS_KERNEL;
extern const char *DIAG_RAM;
extern const char *DIAG_FREE_PREFIX;
extern const char *DIAG_TOTAL_SEPARATOR;
extern const char *DIAG_DISPLAY;
extern const char *DIAG_SDL2_GFX;
extern const char *DIAG_HW_ACCEL;
extern const char *DIAG_SQLITE_DB;
extern const char *DIAG_SCHEMA_VER_PREFIX;
extern const char *DIAG_SD_STORAGE;
extern const char *DIAG_GAMEPAD;
extern const char *DIAG_WIFI;
extern const char *DIAG_SAFETY;
extern const char *DIAG_SAFETY_VAL;
extern const char *DIAG_VAL_64BIT;
extern const char *DIAG_VAL_HW_ACCEL;
extern const char *DIAG_VAL_SCHEMA_PREFIX;

// --- 14. CẬP NHẬT OTA ---
extern const char *OTA_TITLE;
extern const char *OTA_CHECKING;
extern const char *OTA_DEV_CURRENT_VER;
extern const char *OTA_DEV_SOURCE_PREFIX;
extern const char *OTA_STATUS_IDLE;
extern const char *OTA_STATUS_CHECKING;
extern const char *OTA_STATUS_UPDATE_AVAILABLE;
extern const char *OTA_STATUS_UP_TO_DATE;
extern const char *OTA_STATUS_FAILED;
extern const char *OTA_STATUS_DOWNLOADING;
extern const char *OTA_STATUS_VERIFYING;
extern const char *OTA_STATUS_COMPLETED;
extern const char *OTA_MSG_UPDATE_AVAILABLE;
extern const char *OTA_MSG_COMPLETED;
extern const char *OTA_DOWNLOAD_BTN;
extern const char *OTA_CHECK_AGAIN_BTN;
extern const char *OTA_BACK_BTN;

// --- 15. TOAST MESSAGES ---
extern const char *TOAST_ADDED_TO_QUEUE;
extern const char *TOAST_REMOVED_FROM_QUEUE;
extern const char *TOAST_DOWNLOAD_STOPPED;
extern const char *TOAST_GAME_EXISTS_DELETE;
extern const char *TOAST_GAME_ONLY_ON_DRIVE;
extern const char *TOAST_CONNECT_DRIVE_FIRST;
extern const char *TOAST_DELETED_SUCCESS;
extern const char *TOAST_LOGOUT_SUCCESS;
extern const char *TOAST_OTA_CANCELLED;

// --- 16. MULTI-SELECT MODE ---
extern const char *MULTI_SELECT_ENABLED;
extern const char *MULTI_SELECT_DISABLED;
extern const char *MULTI_SELECT_HINT;
extern const char *MULTI_SELECT_COUNT;

// --- 16B. MENU SUBTITLE ---
extern const char *MENU_SUB_PLAY;
extern const char *MENU_SUB_SYNC;
extern const char *MENU_SUB_REVERSE_SYNC;
extern const char *MENU_SUB_OTA;
extern const char *MENU_SUB_OTA_NEW;
extern const char *MENU_SUB_SETTINGS;
extern const char *MENU_SUB_DIAG;
extern const char *MENU_SUB_EXIT;

// --- 16C. SEARCH ---
extern const char *SEARCH_BADGE_LOCAL;
extern const char *SEARCH_BADGE_CLOUD;
extern const char *SEARCH_PROMPT_INPUT;

// --- 17. TOAST (EXTRA) ---
extern const char *TOAST_NEW_OTA_PREFIX;
extern const char *TOAST_SYNCING_DRIVE;
extern const char *TOAST_SCANNING_SD;
extern const char *TOAST_SD_SCANNED_NO_DRIVE;
extern const char *TOAST_UNKNOWN_ERROR;
extern const char *TOAST_SYNC_CANCELLED;
extern const char *TOAST_GOOGLE_LOGIN_SUCCESS;
extern const char *TOAST_CONNECT_PERSONAL_DRIVE;

// --- 17B. BACKUP ---
extern const char *BACKUP_SUCCESS;
extern const char *BACKUP_FAILED;

// --- 18. BACKUP & RESTORE ---
extern const char *BACKUP_TITLE;
extern const char *BACKUP_EXPORT_BTN;
extern const char *BACKUP_IMPORT_BTN;
extern const char *BACKUP_EXPORTING;
extern const char *BACKUP_IMPORTING;
extern const char *BACKUP_EXPORT_SUCCESS;
extern const char *BACKUP_EXPORT_FAILED;
extern const char *BACKUP_RESTORE_SUCCESS;
extern const char *BACKUP_RESTORE_FAILED;
extern const char *BACKUP_NO_FILE;

// --- 19. DASHBOARD ---
extern const char *DASH_TITLE;
extern const char *DASH_DEVICE_LABEL;
extern const char *DASH_DEVICE_VAL;
extern const char *DASH_STORAGE_LABEL;
extern const char *DASH_STORAGE_FREE;
extern const char *DASH_SD_PREFIX;
extern const char *DASH_DRIVE_LABEL;
extern const char *DASH_DRIVE_CONNECTED;
extern const char *DASH_DRIVE_DISCONNECTED;
extern const char *DASH_PORTAL_LABEL;
extern const char *DASH_VERSION_LABEL;
extern const char *DASH_NEW_VERSION_BADGE;

// --- 20. GAME DETAIL ---
extern const char *GAME_LOCATION_SD_PREFIX;
extern const char *GAME_LOCATION_DRIVE;
extern const char *GAME_DOWNLOADING_PREFIX;
extern const char *GAME_QUEUE_DOWNLOADING;
extern const char *GAME_QUEUE_WAITING;
extern const char *GAME_QUEUE_REMAINING;

// --- 21. DIALOG ---
extern const char *DIALOG_DELETE_FILE_PREFIX;

// --- 22. BATCH DELETE ---
extern const char *MULTI_BATCH_DELETE_TITLE;
extern const char *MULTI_BATCH_DELETE_SAFE;
extern const char *MULTI_BATCH_DELETE_CONFIRM;

// --- 23. OTA (EXTRA) ---
extern const char *OTA_WAITING;
extern const char *OTA_MSG_UP_TO_DATE;
extern const char *OTA_NO_NEW_UPDATE;
extern const char *OTA_BTNS_CHECK_BACK;
extern const char *OTA_STATUS_NEW_UPDATE;
extern const char *OTA_DEV_NEW_VER_PREFIX;
extern const char *OTA_CHANGELOG_TITLE;
extern const char *OTA_BTN_INSTALL_NOW;
extern const char *OTA_DOWNLOADING_TITLE;
extern const char *OTA_VERIFYING_FILE;
extern const char *OTA_BTN_CANCEL_DOWNLOAD;
extern const char *OTA_MSG_RESTART_HINT;
extern const char *OTA_BTN_RESTART_NOW;
extern const char *OTA_MSG_FAILED;
extern const char *OTA_ERR_NETWORK;
extern const char *OTA_BTNS_RETRY_BACK;
extern const char *OTA_VIEW_FULL_HINT;

// --- 24. SETTINGS ---
extern const char *BACKUP_EXPORT_DESC;
extern const char *BACKUP_IMPORT_DESC;

// --- ABOUT ---
extern const char *ABOUT_AUTHOR;
extern const char *ABOUT_DONATE_LINE1;
extern const char *ABOUT_DONATE_LINE2;
extern const char *ABOUT_LETTER_TITLE;
extern const char *ABOUT_NOTES_FALLBACK;

// --- DIAG ---
extern const char *DIAG_DEVICE_ID_LABEL;

// --- INFO TAB ---
extern const char *INFO_TAB_ABOUT;
extern const char *INFO_TAB_SYSTEM;

// --- OTA ---
extern const char *OTA_CHANGELOG_HEADER;

// --- SETTINGS TAB ---
extern const char *SETTINGS_TAB_CONFIG;
extern const char *SETTINGS_TAB_PREFS;
extern const char *SETTINGS_TAB_UPDATE;
extern const char *SETTINGS_TAB_WIFI;
} // namespace UiStrings
} // namespace RomCloud
