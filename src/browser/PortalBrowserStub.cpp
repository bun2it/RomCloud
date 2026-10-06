// PortalBrowser stub cho build PC không có Ultralight SDK.
// Mọi hàm no-op: tab Portal hiện "chưa hỗ trợ trên bản PC".
#include "PortalBrowser.h"
#include "../logging/Logger.h"

namespace RomCloud {

PortalBrowser& PortalBrowser::instance() {
    static PortalBrowser inst;
    return inst;
}

bool PortalBrowser::init(const std::string&, const std::string&, int, int) {
    Logger::warn("PortalBrowser: stub (no Ultralight SDK on PC)");
    return false;
}

void PortalBrowser::shutdown() {}

bool PortalBrowser::navigate(const std::string&) { return false; }

bool PortalBrowser::fetchUrl(const std::string&, std::string&, std::string&) {
    return false;
}

bool PortalBrowser::showHtml(const std::string&, const std::string&) {
    return false;
}

bool PortalBrowser::submitFetch(const std::string&, const std::string&,
                                const std::string&, std::string&, std::string&) {
    return false;
}

bool PortalBrowser::update() { return false; }

bool PortalBrowser::copyPixels(unsigned char*, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    return false;
}

void PortalBrowser::pressTab(bool) {}
void PortalBrowser::pressEnter() {}
void PortalBrowser::pressArrow(int, int) {}
void PortalBrowser::typeText(const std::string&) {}

std::string PortalBrowser::pollNav() { return ""; }

std::vector<PortalConsoleMsg> PortalBrowser::drainConsole() { return {}; }

void PortalBrowser::storeCookies(const std::string&) {}
std::string PortalBrowser::cookieHeader() const { return ""; }
std::string PortalBrowser::fetchPage(const std::string&, std::string&) { return ""; }
std::string PortalBrowser::resolveUrl(const std::string&, const std::string&) { return ""; }
std::string PortalBrowser::inlineResources(const std::string&, const std::string&) { return ""; }

} // namespace RomCloud
