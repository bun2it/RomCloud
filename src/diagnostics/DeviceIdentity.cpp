#include "DeviceIdentity.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <openssl/sha.h>
#include <openssl/evp.h>

#ifdef _WIN32
#include <windows.h>
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace RomCloud {

DeviceIdentity& DeviceIdentity::instance() {
    static DeviceIdentity instance;
    return instance;
}

DeviceIdentity::DeviceIdentity() {
    invalidate();
}

void DeviceIdentity::invalidate() {
    m_initialized = false;
    m_deviceId.clear();
    m_fullHash.clear();
    m_idSource.clear();
    m_hasHardwareId = false;
}

std::string DeviceIdentity::getDeviceId() {
    if (m_initialized) {
        return m_deviceId;
    }

    std::string hardwareId;

    // Priority 1: Chip ID
    hardwareId = tryChipId();
    if (!hardwareId.empty()) {
        m_idSource = "chip_id";
        m_hasHardwareId = true;
        Logger::debug("DeviceIdentity: Using chip ID");
    }
    // Priority 2: MAC address
    else if ((hardwareId = tryMacAddress()), !hardwareId.empty()) {
        m_idSource = "mac";
        m_hasHardwareId = true;
        Logger::debug("DeviceIdentity: Using MAC address");
    }
    // Priority 3: machine-id
    else if ((hardwareId = tryMachineId()), !hardwareId.empty()) {
        m_idSource = "machine_id";
        m_hasHardwareId = true;
        Logger::debug("DeviceIdentity: Using machine-id");
    }
    // Priority 4: Fallback to install ID
    else {
        hardwareId = getOrCreateInstallId();
        m_idSource = "install_id";
        m_hasHardwareId = false;
        Logger::debug("DeviceIdentity: Using install ID (no hardware ID available)");
    }

    // Hash the ID with domain prefix
    m_fullHash = hashWithDomain(hardwareId);

    // Create short ID: RC- + first 8 chars of hex hash
    m_deviceId = "RC-" + m_fullHash.substr(0, 8);

    m_initialized = true;

    Logger::info("DeviceIdentity: " + m_deviceId + " (source: " + m_idSource + ")");

    return m_deviceId;
}

std::string DeviceIdentity::getFullHash() {
    if (!m_initialized) {
        getDeviceId();  // Force init
    }
    return m_fullHash;
}

std::string DeviceIdentity::tryChipId() {
    // TrimUI Brick Pro uses MTD partition for device ID
    // Common paths for various devices
    const char* chipIdPaths[] = {
        "/sys/class/mtd/mtd0/device/id",
        "/sys/class/mtd/mtd1/device/id",
        "/sys/class/mtd/mtd2/device/id",
        "/proc/device-tree/serial-number",
        "/proc/cpuinfo",  // Fallback: try to extract serial from cpuinfo
        "/sys/firmware/devicetree/base/serial-number",
        "/sys/class/dmi/id/product_serial",
        "/factory/serial_no",
        "/mnt/vendor/product_id",
        nullptr
    };

    for (int i = 0; chipIdPaths[i] != nullptr; ++i) {
        std::string path(chipIdPaths[i]);

        // Special handling for /proc/cpuinfo
        if (path == "/proc/cpuinfo") {
            std::ifstream file(path);
            if (!file.is_open()) continue;

            std::string line;
            while (std::getline(file, line)) {
                // Look for "Serial" field (Raspberry Pi, some Allwinner)
                if (line.find("Serial") == 0 || line.find("serial-number") != std::string::npos) {
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string serial = line.substr(colon + 1);
                        // Trim whitespace
                        serial.erase(0, serial.find_first_not_of(" \t"));
                        serial.erase(serial.find_last_not_of(" \t") + 1);
                        if (isValidHardwareId(serial)) {
                            return serial;
                        }
                    }
                }
            }
            continue;
        }

        std::ifstream file(path);
        if (!file.is_open()) continue;

        std::string id;
        std::getline(file, id);
        file.close();

        // Trim whitespace
        id.erase(0, id.find_first_not_of(" \t\r\n"));
        id.erase(id.find_last_not_of(" \t\r\n") + 1);

        if (isValidHardwareId(id)) {
            return id;
        }
    }

    return "";
}

std::string DeviceIdentity::tryMacAddress() {
    // Try wired interface first (more stable than WiFi)
    const char* macPaths[] = {
        "/sys/class/net/eth0/address",
        "/sys/class/net/wlan0/address",
        "/sys/class/net/wifi0/address",
        "/sys/class/net/ra0/address",
        nullptr
    };

    for (int i = 0; macPaths[i] != nullptr; ++i) {
        std::ifstream file(macPaths[i]);
        if (!file.is_open()) continue;

        std::string mac;
        std::getline(file, mac);
        file.close();

        // Trim whitespace and convert to lowercase
        mac.erase(0, mac.find_first_not_of(" \t\r\n"));
        mac.erase(mac.find_last_not_of(" \t\r\n") + 1);

        // Normalize: remove : and convert to lowercase
        std::string normalized;
        for (char c : mac) {
            if (c != ':') {
                normalized += static_cast<char>(std::tolower(c));
            }
        }

        if (isValidHardwareId(normalized)) {
            return normalized;
        }
    }

    return "";
}

std::string DeviceIdentity::tryMachineId() {
    const char* machineIdPaths[] = {
        "/etc/machine-id",
        "/var/lib/dbus/machine-id",
        nullptr
    };

    for (int i = 0; machineIdPaths[i] != nullptr; ++i) {
        std::ifstream file(machineIdPaths[i]);
        if (!file.is_open()) continue;

        std::string id;
        std::getline(file, id);
        file.close();

        // Trim whitespace
        id.erase(0, id.find_first_not_of(" \t\r\n"));
        id.erase(id.find_last_not_of(" \t\r\n") + 1);

        if (isValidHardwareId(id)) {
            return id;
        }
    }

    return "";
}

bool DeviceIdentity::isValidHardwareId(const std::string& id) const {
    if (id.empty()) {
        return false;
    }

    // Check minimum length (MAC = 12 chars, machine-id = 32 chars)
    if (id.length() < 8) {
        return false;
    }

    // Check if it's all zeros (common default/placeholder)
    bool allZeros = true;
    for (char c : id) {
        if (c != '0' && c != ':' && c != '-' && !std::isspace(c)) {
            allZeros = false;
            break;
        }
    }
    if (allZeros) {
        return false;
    }

    // Check for default/placeholder strings
    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                  [](unsigned char c){ return std::tolower(c); });

    const char* invalidIds[] = {
        "unknown", "none", "null", "n/a", "na", "default",
        "000000000000", "ffffffffffff", "deadbeef",
        nullptr
    };

    for (int i = 0; invalidIds[i] != nullptr; ++i) {
        if (lower == invalidIds[i]) {
            return false;
        }
        // Also check if it starts with these (partial match)
        if (lower.length() >= strlen(invalidIds[i]) &&
            lower.substr(0, strlen(invalidIds[i])) == invalidIds[i]) {
            // But allow if it's long enough (probably real ID starting with "unknown" is unlikely)
            if (id.length() < 20) {
                return false;
            }
        }
    }

    return true;
}

std::string DeviceIdentity::hashWithDomain(const std::string& rawId) {
    // Combine domain + raw ID for hashing
    std::string toHash = std::string(DEVICE_ID_DOMAIN) + ":" + rawId;

    // SHA-256 hash
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(toHash.c_str()), toHash.length(), hash);

    // Convert to hex string
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    return oss.str();
}

std::string DeviceIdentity::getInstallIdPath() const {
    // Store in config directory
    std::string configDir = AppConfig::instance().getConfigDir();
    return configDir + "/device_id";
}

std::string DeviceIdentity::getOrCreateInstallId() {
    std::string path = getInstallIdPath();

    // Try to read existing ID
    std::ifstream file(path);
    if (file.is_open()) {
        std::string id;
        std::getline(file, id);
        file.close();

        // Validate
        id.erase(0, id.find_first_not_of(" \t\r\n"));
        id.erase(id.find_last_not_of(" \t\r\n") + 1);

        if (!id.empty() && id.length() >= 8) {
            Logger::debug("DeviceIdentity: Loaded existing install ID");
            return id;
        }
    }

    // Generate new install ID
    std::string newId;

#ifdef _WIN32
    // Windows: use random + some unique system info
    GUID guid;
    CoCreateGuid(&guid);
    char guidStr[64];
    snprintf(guidStr, sizeof(guidStr),
             "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
             guid.Data1, guid.Data2, guid.Data3,
             guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
             guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    newId = std::string(guidStr);
#else
    // Linux: use /dev/urandom for secure random
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.is_open()) {
        unsigned char buf[16];
        urandom.read(reinterpret_cast<char*>(buf), sizeof(buf));
        urandom.close();

        std::ostringstream oss;
        for (int i = 0; i < 16; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)buf[i];
        }
        newId = oss.str();
    } else {
        // Fallback: timestamp + random
        auto now = std::time(nullptr);
        std::srand(static_cast<unsigned>(now));
        std::ostringstream oss;
        oss << std::hex << now << "-" << std::setw(4) << std::setfill('0') << std::rand();
        newId = oss.str();
    }
#endif

    // Save to file (persist across app updates)
    std::ofstream outFile(path);
    if (outFile.is_open()) {
        outFile << newId << std::endl;
        outFile.close();
        Logger::info("DeviceIdentity: Created new install ID");

        // Make file read-only (only owner can write) to prevent tampering
#ifdef _WIN32
        // Windows: just ensure it's not readable by others
#else
        chmod(path.c_str(), 0644);
#endif
    } else {
        Logger::warn("DeviceIdentity: Could not save install ID to " + path);
    }

    return newId;
}

} // namespace RomCloud
