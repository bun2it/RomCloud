#pragma once
#include <string>

namespace RomCloud {

/**
 * Device Identity Module
 *
 * Provides a stable, privacy-safe device identifier for bug reporting.
 *
 * Hardware ID Priority:
 * 1. Chip/Serial ID (MTD flash)
 * 2. MAC address (eth0)
 * 3. machine-id (Linux)
 * 4. Fallback: Persistent install ID
 *
 * Output: RC-xxxxxxxx (8-char hash, never raw hardware IDs)
 */
class DeviceIdentity {
public:
    static DeviceIdentity& instance();

    /**
     * Get the stable device identifier.
     * Format: RC-<8-char-hash>
     *
     * @return Device ID string, never empty
     */
    std::string getDeviceId();

    /**
     * Get the full 64-char SHA-256 hash (for internal use).
     */
    std::string getFullHash();

    /**
     * Check if this device has a hardware-based ID (vs install-based).
     */
    bool hasHardwareId() const { return m_hasHardwareId; }

    /**
     * Get source of the ID (for diagnostics).
     * Returns: "chip_id", "mac", "machine_id", or "install_id"
     */
    const std::string& getIdSource() const { return m_idSource; }

    /**
     * Invalidate cached ID (force re-detection).
     */
    void invalidate();

private:
    DeviceIdentity();
    ~DeviceIdentity() = default;
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;

    // Try each hardware source in priority order
    std::string tryChipId();
    std::string tryMacAddress();
    std::string tryMachineId();

    // Validate hardware ID (not empty, not default, not all zeros)
    bool isValidHardwareId(const std::string& id) const;

    // Hash with domain string for isolation
    std::string hashWithDomain(const std::string& rawId);

    // Generate/load persistent install ID
    std::string getOrCreateInstallId();

    // Settings paths
    std::string getInstallIdPath() const;

    std::string m_deviceId;
    std::string m_fullHash;
    std::string m_idSource;
    bool m_hasHardwareId = false;
    bool m_initialized = false;
};

// App-specific domain prefix for SHA-256 hashing
// Different apps should use different prefixes to prevent correlation
constexpr const char* DEVICE_ID_DOMAIN = "RomCloud-v2";  // Versioned per app

} // namespace RomCloud
