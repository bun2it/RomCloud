// OneDrive public share-link sync (no credentials needed).
// Uses api.onedrive.com shares API: GET /shares/{token}/driveItem/children
// Game cloudFileId format: "od1|<shareToken>|<itemId>" resolved to
// @microsoft.graph.downloadUrl at download time (see DownloadManager).
#pragma once
#include <string>

namespace RomCloud {

// Encode a share URL into an api.onedrive.com share token ("u!..." base64url).
std::string oneDriveEncodeShareToken(const std::string& shareUrl);
// True if url looks like a OneDrive/SharePoint share link.
bool oneDriveIsShareLink(const std::string& url);
// Full sync: list share folder recursively and index ROMs into DB. Returns games indexed (<0 on error).
int oneDriveSyncShare(const std::string& shareUrl);

} // namespace RomCloud
