#include <iostream>
#include <cassert>
#include "../src/network/JsonHelper.h"
#include "../src/ui/qrcodegen.hpp"

using namespace RomCloud;

int main() {
    std::cout << "[TEST] Starting RomCloud Phase 4 Validation Suite..." << std::endl;

    // 1. Test JsonHelper extraction
    std::string sampleJson = R"({
        "device_code": "AGY_DEV_CODE_12345",
        "user_code": "WDJB-MJHT",
        "verification_url": "https://www.google.com/device",
        "expires_in": 1800,
        "interval": 5,
        "email": "gamer@gmail.com"
    })";

    std::string devCode = JsonHelper::extractString(sampleJson, "device_code");
    std::string userCode = JsonHelper::extractString(sampleJson, "user_code");
    std::string url = JsonHelper::extractString(sampleJson, "verification_url");
    std::string email = JsonHelper::extractString(sampleJson, "email");
    int expiresIn = JsonHelper::extractInt(sampleJson, "expires_in");
    int interval = JsonHelper::extractInt(sampleJson, "interval");

    assert(devCode == "AGY_DEV_CODE_12345" && "device_code failed");
    assert(userCode == "WDJB-MJHT" && "user_code failed");
    assert(url == "https://www.google.com/device" && "verification_url failed");
    assert(email == "gamer@gmail.com" && "email failed");
    assert(expiresIn == 1800 && "expires_in failed");
    assert(interval == 5 && "interval failed");
    std::cout << "[PASS] JSON extraction of OAuth parameters verified." << std::endl;

    // 2. Test QR Code Generation
    std::string qrTarget = "https://www.google.com/device?user_code=WDJB-MJHT";
    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(qrTarget.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    assert(qr.getSize() > 0 && "QR code generation failed");
    std::cout << "[PASS] QR Code matrix generated successfully (Version " << qr.getVersion() << ", Size " << qr.getSize() << "x" << qr.getSize() << ")." << std::endl;

    std::cout << "[SUCCESS] RomCloud Phase 4 Validation Suite PASSED!" << std::endl;
    return 0;
}
