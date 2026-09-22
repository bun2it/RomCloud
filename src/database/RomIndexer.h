#pragma once
#include "DatabaseManager.h"
#include <string>
#include <vector>
#include <functional>

namespace RomCloud {

struct ScanProgress {
    std::string currentSystem;
    std::string currentFile;
    int filesFound = 0;
    int filesAdded = 0;
    int filesUpdated = 0;
    int filesMissing = 0;
};

using ScanProgressCallback = std::function<void(const ScanProgress&)>;

class RomIndexer {
public:
    static RomIndexer& instance();
    ScanProgress scanAllSystems(const std::string& romsBaseDir, ScanProgressCallback progressCb = nullptr);
    ScanProgress scanSystem(const SystemRecord& system, const std::string& romsBaseDir, ScanProgressCallback progressCb = nullptr);

    static std::string cleanTitleFromFilename(const std::string& filename);
    static bool isExtensionSupported(const std::string& filename, const std::string& extList);

private:
    RomIndexer() = default;
};

} // namespace RomCloud
