#pragma once
// BrickPadProtocol.h — HID Classic descriptor + report packer, port từ
// BrickPad (BrickPad.h + brickpad.c §7). Code BrickPad do Minh viết, không
// vướng bản quyền. Descriptor giữ nguyên để TV/PC/Mac nhận chuẩn.
#include <cstdint>
#include <cstring>

namespace RomCloud {
namespace BrickPad {

enum class ControllerIdentity {
    BRICKPAD = 0,
    XBOX = 1,
    PLAYSTATION = 2,
    SWITCH = 3,
    PC_LAPTOP = 4
};

struct IdentityInfo {
    const char *name;
    const char *btName;
    uint16_t vid;
    uint16_t pid;
    uint16_t version;
    const char *desc;
};

inline IdentityInfo getIdentityInfo(ControllerIdentity id) {
    switch (id) {
    case ControllerIdentity::PC_LAPTOP:
        return {"PC/Laptop", "BrickPro Desk Remote", 0x046D, 0xB342, 0x0100, "BrickPro Desk Remote (Logitech Combo)"};
    case ControllerIdentity::XBOX:
    case ControllerIdentity::BRICKPAD:
    case ControllerIdentity::PLAYSTATION:
    case ControllerIdentity::SWITCH:
    default:
        // Chuẩn Xbox Wireless Controller (Model 1708) qua Bluetooth HID:
        // VID: 0x045E (Microsoft), PID: 0x02FD (Model 1708), CoD: 0x002508
        return {"Xbox", "Xbox Wireless Controller", 0x045E, 0x02FD, 0x0100, "Xbox Wireless Controller (Model 1708)"};
    }
}

// --- Report Descriptor: Sony DualShock 4 (Report ID 1) + Keyboard (ID 2) + Mouse (ID 3) ---
// Report ID 1 (10 bytes):
//   Byte 0: 0x01 (Report ID)
//   Byte 1: Left Stick X (0..255, 128 center)
//   Byte 2: Left Stick Y (0..255, 128 center)
//   Byte 3: Right Stick X (0..255, 128 center)
//   Byte 4: Right Stick Y (0..255, 128 center)
//   Byte 5: D-pad Hat (bits 0..3: 0=Up, 2=Right, 4=Down, 6=Left, 8=neutral)
//           Face buttons (bits 4..7: bit 4=Square, bit 5=Cross, bit 6=Circle, bit 7=Triangle)
//   Byte 6: L1(0), R1(1), L2(2), R2(3), Share(4), Options(5), L3(6), R3(7)
//   Byte 7: PS(0), Touchpad Click(1), padding(bits 2..7)
//   Byte 8: L2 trigger analog (0..255)
//   Byte 9: R2 trigger analog (0..255)
static const uint8_t kReportMapDS4[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x30,        //   Usage (X) - Left Stick X
    0x09, 0x31,        //   Usage (Y) - Left Stick Y
    0x09, 0x32,        //   Usage (Z) - Right Stick X
    0x09, 0x35,        //   Usage (Rz) - Right Stick Y
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (Eng Rot:Angular Pos)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null)
    0x65, 0x00,        //   Unit (None)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (Button 1: Square)
    0x29, 0x04,        //   Usage Maximum (Button 4: Triangle)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x19, 0x05,        //   Usage Minimum (Button 5: L1)
    0x29, 0x0C,        //   Usage Maximum (Button 12: R3)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x19, 0x0D,        //   Usage Minimum (Button 13: PS)
    0x29, 0x0E,        //   Usage Maximum (Button 14: Touchpad Click)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const,Ary,Abs) - Padding 6 bits
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x33,        //   Usage (Rx) - L2 Trigger analog
    0x09, 0x34,        //   Usage (Ry) - R2 Trigger analog
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0xC0,              // End Collection
    // --- Keyboard (Report ID 2) ---
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x07, 0x19, 0xE0,
    0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xC0,
    // --- Mouse (Report ID 3) ---
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x06, 0xC0, 0xC0
};

// --- Report Descriptor: Xbox Wireless Controller Model 1708 (Report ID 1) + Keyboard + Mouse ---
static const uint8_t kReportMapXbox[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x30,        //     Usage (X) - Left Stick X
    0x09, 0x31,        //     Usage (Y) - Left Stick Y
    0x15, 0x00,        //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical Maximum (65535)
    0x95, 0x02,        //     Report Count (2)
    0x75, 0x10,        //     Report Size (16)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x32,        //     Usage (Z) - Right Stick X
    0x09, 0x35,        //     Usage (Rz) - Right Stick Y
    0x15, 0x00,        //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical Maximum (65535)
    0x95, 0x02,        //     Report Count (2)
    0x75, 0x10,        //     Report Size (16)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0x05, 0x02,        //   Usage Page (Simulation Ctrls)
    0x09, 0xC5,        //   Usage (Brake) - Left Trigger
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x0A,        //   Report Size (10)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x09, 0xC4,        //   Usage (Accelerator) - Right Trigger
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x0A,        //   Report Size (10)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x01,        //   Logical Minimum (1)
    0x25, 0x08,        //   Logical Maximum (8)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x66, 0x14, 0x00,  //   Unit (Eng Rot:Angular Pos)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x35, 0x00,        //   Physical Minimum (0)
    0x45, 0x00,        //   Physical Maximum (0)
    0x65, 0x00,        //   Unit (None)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (Button 1: A)
    0x29, 0x0A,        //   Usage Maximum (Button 10: R3)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0A,        //   Report Count (10)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0xC0,              // End Collection
    // --- Keyboard (Report ID 2) ---
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x07, 0x19, 0xE0,
    0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xC0,
    // --- Mouse (Report ID 3) ---
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x06, 0xC0, 0xC0
};

// Generic / Switch Pro descriptor (Report ID 1, 8-byte layout)
static const uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x39, 0x15, 0x00,
    0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95,
    0x01, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01, 0x65, 0x00, 0x35,
    0x00, 0x45, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09,
    0x31, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08,
    0x95, 0x04, 0x81, 0x02, 0xC0,
    // --- Keyboard (Report ID 2) ---
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x07, 0x19, 0xE0,
    0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xC0,
    // --- Mouse (Report ID 3) ---
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x06, 0xC0, 0xC0
};

// --- Report Descriptor: Composite Remote (Mouse ID 1 + Keyboard ID 2 + Consumer ID 3) ---
static const uint8_t kReportMapPCLaptop[] = {
    // --------------------------------------------------------
    // COLLECTION 1: CHUỘT (Report ID 0x01)
    // --------------------------------------------------------
    0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,        // USAGE (Mouse)
    0xA1, 0x01,        // COLLECTION (Application)
    0x85, 0x01,        //   REPORT_ID (1)
    0x09, 0x01,        //   USAGE (Pointer)
    0xA1, 0x00,        //   COLLECTION (Physical)
    0x05, 0x09,        //     USAGE_PAGE (Button)
    0x19, 0x01,        //     USAGE_MINIMUM (Button 1 - Trái)
    0x29, 0x03,        //     USAGE_MAXIMUM (Button 3 - Giữa)
    0x15, 0x00,        //     LOGICAL_MINIMUM (0)
    0x25, 0x01,        //     LOGICAL_MAXIMUM (1)
    0x95, 0x03,        //     REPORT_COUNT (3)
    0x75, 0x01,        //     REPORT_SIZE (1)
    0x81, 0x02,        //     INPUT (Data,Var,Abs)
    0x95, 0x01,        //     REPORT_COUNT (1)
    0x75, 0x05,        //     REPORT_SIZE (5) - Padding
    0x81, 0x03,        //     INPUT (Cnst,Var,Abs)
    0x05, 0x01,        //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,        //     USAGE (X)
    0x09, 0x31,        //     USAGE (Y)
    0x15, 0x81,        //     LOGICAL_MINIMUM (-127)
    0x25, 0x7F,        //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,        //     REPORT_SIZE (8)
    0x95, 0x02,        //     REPORT_COUNT (2)
    0x81, 0x06,        //     INPUT (Data,Var,Rel)
    0x09, 0x38,        //     USAGE (Wheel - Con lăn cuộn)
    0x15, 0x81,        //     LOGICAL_MINIMUM (-127)
    0x25, 0x7F,        //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,        //     REPORT_SIZE (8)
    0x95, 0x01,        //     REPORT_COUNT (1)
    0x81, 0x06,        //     INPUT (Data,Var,Rel)
    0xC0,              //   END_COLLECTION
    0xC0,              // END_COLLECTION

    // --------------------------------------------------------
    // COLLECTION 2: BÀN PHÍM BOOT PROTOCOL (Report ID 0x02)
    // --------------------------------------------------------
    0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,        // USAGE (Keyboard)
    0xA1, 0x01,        // COLLECTION (Application)
    0x85, 0x02,        //   REPORT_ID (2)
    0x05, 0x07,        //   USAGE_PAGE (Keyboard)
    0x19, 0xE0,        //   USAGE_MINIMUM (Left Control)
    0x29, 0xE7,        //   USAGE_MAXIMUM (Right GUI)
    0x15, 0x00,        //   LOGICAL_MINIMUM (0)
    0x25, 0x01,        //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,        //   REPORT_SIZE (1)
    0x95, 0x08,        //   REPORT_COUNT (8)
    0x81, 0x02,        //   INPUT (Data,Var,Abs) - Modifiers
    0x95, 0x01,        //   REPORT_COUNT (1)
    0x75, 0x08,        //   REPORT_SIZE (8)
    0x81, 0x03,        //   INPUT (Cnst,Var,Abs) - Reserved
    0x95, 0x06,        //   REPORT_COUNT (6)
    0x75, 0x08,        //   REPORT_SIZE (8)
    0x15, 0x00,        //   LOGICAL_MINIMUM (0)
    0x25, 0x65,        //   LOGICAL_MAXIMUM (101)
    0x05, 0x07,        //   USAGE_PAGE (Keyboard)
    0x19, 0x00,        //   USAGE_MINIMUM (0)
    0x29, 0x65,        //   USAGE_MAXIMUM (101)
    0x81, 0x00,        //   INPUT (Data,Ary,Abs) - 6 Keycodes
    0xC0,              // END_COLLECTION

    // --------------------------------------------------------
    // COLLECTION 3: ĐIỀU KHIỂN ĐA PHƯƠNG TIỆN (Report ID 0x03)
    // --------------------------------------------------------
    0x05, 0x0C,        // USAGE_PAGE (Consumer Devices)
    0x09, 0x01,        // USAGE (Consumer Control)
    0xA1, 0x01,        // COLLECTION (Application)
    0x85, 0x03,        //   REPORT_ID (3)
    0x15, 0x00,        //   LOGICAL_MINIMUM (0)
    0x26, 0x9C, 0x02,  //   LOGICAL_MAXIMUM (668)
    0x19, 0x00,        //   USAGE_MINIMUM (0)
    0x2A, 0x9C, 0x02,  //   USAGE_MAXIMUM (668)
    0x75, 0x10,        //   REPORT_SIZE (16)
    0x95, 0x01,        //   REPORT_COUNT (1)
    0x81, 0x00,        //   INPUT (Data,Ary,Abs)
    0xC0               // END_COLLECTION
};

inline const uint8_t *getReportMap(ControllerIdentity id, uint16_t &len) {
    if (id == ControllerIdentity::PC_LAPTOP) {
        len = (uint16_t)sizeof(kReportMapPCLaptop);
        return kReportMapPCLaptop;
    } else if (id == ControllerIdentity::PLAYSTATION) {
        len = (uint16_t)sizeof(kReportMapDS4);
        return kReportMapDS4;
    } else {
        // Chuẩn Xbox Wireless Controller (Model 1708) mặc định
        len = (uint16_t)sizeof(kReportMapXbox);
        return kReportMapXbox;
    }
}
static const uint16_t kReportMapLen = (uint16_t)sizeof(kReportMapDS4);

// Thứ tự nút vật lý (khớp nhãn trên máy Brick, khớp InputManager Button).
enum PhysBtn {
    P_A = 0, P_B, P_X, P_Y, P_L1, P_R1, P_L2, P_R2,
    P_SEL, P_START, P_L3, P_R3, P_HOME, P_FN1, P_FN2, P_COUNT
};
static const char *kPhysKeys[P_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2",
    "SEL", "START", "L3", "R3", "HOME", "FN1", "FN2"};
static const char *kPhysLabels[P_COUNT] = {
    "A (phải)", "B (dưới)", "X (trên)", "Y (trái)",
    "L1", "R1", "L2", "R2", "SEL", "START", "L3", "R3",
    "HOME (Menu)", "FN1", "FN2"};
static const char *kHidBitNames[16] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2",
    "SEL", "START", "L3", "R3", "HOME", "FN1", "FN2", "bit15"};

// Trạng thái nút vật lý (đọc từ InputManager, không grab evdev vì RomCloud
// đã sở hữu input qua SDL).
struct BrickInput {
    bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
    bool btnA = false, btnB = false, btnX = false, btnY = false;
    bool btnL1 = false, btnR1 = false, btnL2 = false, btnR2 = false;
    bool btnSelect = false, btnStart = false;
    bool btnL3 = false, btnR3 = false;
    bool btnHome = false, btnFN1 = false, btnFN2 = false;
    uint8_t stickLX = 128, stickLY = 128, stickRX = 128, stickRY = 128;
};

inline uint8_t hatOf(bool up, bool down, bool left, bool right) {
    if (up && right) return 1;
    if (right && down) return 3;
    if (down && left) return 5;
    if (left && up) return 7;
    if (up) return 0;
    if (right) return 2;
    if (down) return 4;
    if (left) return 6;
    return 8;
}

inline void physFromInput(const BrickInput &in, bool p[P_COUNT]) {
    p[P_A] = in.btnA; p[P_B] = in.btnB; p[P_X] = in.btnX; p[P_Y] = in.btnY;
    p[P_L1] = in.btnL1; p[P_R1] = in.btnR1;
    p[P_L2] = in.btnL2; p[P_R2] = in.btnR2;
    p[P_SEL] = in.btnSelect; p[P_START] = in.btnStart;
    p[P_L3] = in.btnL3; p[P_R3] = in.btnR3;
    p[P_HOME] = in.btnHome; p[P_FN1] = in.btnFN1; p[P_FN2] = in.btnFN2;
}

inline size_t packGamepadReport(ControllerIdentity id, const BrickInput &in, uint8_t *out, size_t maxLen) {
    if (!out || maxLen < 8) return 0;
    if (id == ControllerIdentity::PLAYSTATION) {
        if (maxLen < 10) return 0;
        out[0] = 0x01; // Report ID 1
        out[1] = in.stickLX;
        out[2] = in.stickLY;
        out[3] = in.stickRX;
        out[4] = in.stickRY;
        uint8_t hat = hatOf(in.dpadUp, in.dpadDown, in.dpadLeft, in.dpadRight);
        uint8_t face = 0;
        if (in.btnY) face |= (1 << 4); // Square (Left)
        if (in.btnB) face |= (1 << 5); // Cross (Bottom)
        if (in.btnA) face |= (1 << 6); // Circle (Right)
        if (in.btnX) face |= (1 << 7); // Triangle (Top)
        out[5] = (hat & 0x0F) | face;
        uint8_t b6 = 0;
        if (in.btnL1)     b6 |= (1 << 0);
        if (in.btnR1)     b6 |= (1 << 1);
        if (in.btnL2)     b6 |= (1 << 2);
        if (in.btnR2)     b6 |= (1 << 3);
        if (in.btnSelect) b6 |= (1 << 4); // Share
        if (in.btnStart)  b6 |= (1 << 5); // Options
        if (in.btnL3)     b6 |= (1 << 6);
        if (in.btnR3)     b6 |= (1 << 7);
        out[6] = b6;
        out[7] = in.btnHome ? 0x01 : 0x00; // PS
        out[8] = in.btnL2 ? 255 : 0; // L2 analog
        out[9] = in.btnR2 ? 255 : 0; // R2 analog
        return 10;
    } else {
        // Chuẩn Xbox Wireless Controller (Model 1708) mặc định (16 bytes report)
        if (maxLen < 16) return 0;
        out[0] = 0x01;
        uint16_t lx = ((uint16_t)in.stickLX << 8) | in.stickLX;
        uint16_t ly = ((uint16_t)in.stickLY << 8) | in.stickLY;
        uint16_t rx = ((uint16_t)in.stickRX << 8) | in.stickRX;
        uint16_t ry = ((uint16_t)in.stickRY << 8) | in.stickRY;
        out[1] = (uint8_t)(lx & 0xFF); out[2] = (uint8_t)(lx >> 8);
        out[3] = (uint8_t)(ly & 0xFF); out[4] = (uint8_t)(ly >> 8);
        out[5] = (uint8_t)(rx & 0xFF); out[6] = (uint8_t)(rx >> 8);
        out[7] = (uint8_t)(ry & 0xFF); out[8] = (uint8_t)(ry >> 8);
        uint16_t lt = in.btnL2 ? 1023 : 0;
        uint16_t rt = in.btnR2 ? 1023 : 0;
        out[9] = (uint8_t)(lt & 0xFF); out[10] = (uint8_t)((lt >> 8) & 0x03);
        out[11] = (uint8_t)(rt & 0xFF); out[12] = (uint8_t)((rt >> 8) & 0x03);
        uint8_t h = hatOf(in.dpadUp, in.dpadDown, in.dpadLeft, in.dpadRight);
        uint8_t xh = (h == 8) ? 0 : (h + 1);
        out[13] = xh & 0x0F;
        uint16_t btns = 0;
        if (in.btnB)      btns |= (1 << 0); // A (Bottom)
        if (in.btnA)      btns |= (1 << 1); // B (Right)
        if (in.btnY)      btns |= (1 << 2); // X (Left)
        if (in.btnX)      btns |= (1 << 3); // Y (Top)
        if (in.btnL1)     btns |= (1 << 4); // LB
        if (in.btnR1)     btns |= (1 << 5); // RB
        if (in.btnSelect) btns |= (1 << 6); // View
        if (in.btnStart)  btns |= (1 << 7); // Menu
        if (in.btnL3)     btns |= (1 << 8); // L3
        if (in.btnR3)     btns |= (1 << 9); // R3
        out[14] = (uint8_t)(btns & 0xFF);
        out[15] = (uint8_t)(btns >> 8);
        return 16;
    }
}

inline size_t packNeutralReport(ControllerIdentity id, uint8_t *out, size_t maxLen) {
    BrickInput neutral;
    return packGamepadReport(id, neutral, out, maxLen);
}

// Usage HID cho ký tự Latin (HID Usage Tables, US layout).
// Trả về usage id, mod ra byte modifiers (0x02 = Shift trái). -1 = không hỗ trợ.
inline int kbUsageFor(char c, bool shift, uint8_t &mod) {
    mod = 0;
    if (c >= 'a' && c <= 'z') { if (shift) mod = 0x02; return 0x04 + (c - 'a'); }
    if (c >= 'A' && c <= 'Z') { mod = 0x02; return 0x04 + (c - 'A'); }
    if (c >= '1' && c <= '9') { if (shift) mod = 0x02; return 0x1E + (c - '1'); }
    if (c == '0') { if (shift) mod = 0x02; return 0x27; }
    switch (c) {
    case ' ': return 0x2C;
    case ',': if (shift) mod = 0x02; return 0x36;
    case '.': if (shift) mod = 0x02; return 0x37;
    case '\'': if (shift) mod = 0x02; return 0x34;
    case '-': if (shift) mod = 0x02; return 0x2D;
    case '/': if (shift) mod = 0x02; return 0x38;
    case '!': mod = 0x02; return 0x1E;
    case '?': mod = 0x02; return 0x38;
    default: return -1;
    }
}

// Remote mặc định chế độ Keyboard: nút Brick -> usage HID.
enum : uint8_t {
    KU_ENTER = 0x28, KU_ESC = 0x29, KU_BKSP = 0x2A, KU_SPACE = 0x2C,
    KU_PGUP = 0x4B, KU_PGDN = 0x4E,
    KU_RIGHT = 0x4F, KU_LEFT = 0x50, KU_DOWN = 0x51, KU_UP = 0x52
};

// Consumer Control Usages (Page 0x0C) cho Media Remote:
enum : uint16_t {
    CU_NONE       = 0x0000,
    CU_VOL_UP     = 0x00E9,
    CU_VOL_DOWN   = 0x00EA,
    CU_HOME       = 0x0223,
    CU_MUTE       = 0x00E2,
    CU_PLAY_PAUSE = 0x00CD
};

// Bàn phím ảo: QWERTY 4 hàng + hàng đặc biệt.
static const char *kOskRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const int kOskNumRows = 4;
static const char *kOskSpecial[] = {"Shift", "Space", "Xóa", "Enter", "Đóng"};
static const int kOskNumSpec = 5;
inline int oskCols(int r) {
    if (r < kOskNumRows) return (int)std::strlen(kOskRows[r]);
    return kOskNumSpec;
}

} // namespace BrickPad
} // namespace RomCloud
