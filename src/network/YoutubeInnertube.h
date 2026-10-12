#pragma once

#include <string>
#include <vector>

namespace RomCloud {

// YoutubeInnertube: tim kiem YouTube qua API ngam innertube (POST
// youtubei/v1/search), khong can yt-dlp/python. Nhanh (~2-3s, chi ton
// 1 request HTTPS) de hien ket qua truoc; that bai -> tra ve rong de
// code goi roi xuong duong yt-dlp cu.
//
// Moi dong ket qua: "id|title|durSec|uploader|views" (khop dinh dang
// youtube_search.sh hien tai; '|' trong text da thay bang khoang trang).
class YoutubeInnertube {
public:
    // query thuong. Tien to dac biet: "feed:<cat>" -> trending (browse
    // FEtrending hoac fallback search nhu backend cu); "channel:<id>" ->
    // rong (de duong cu .py xu ly channel).
    static std::vector<std::string> search(const std::string& query,
                                           int maxResults,
                                           const std::string& params = "");

    static std::vector<std::string> trending(const std::string& categoryId,
                                             int maxResults);

    // Resolve stream thang qua player API (khong can yt-dlp).
    // Tra ve "videoURL" (progressive, co san audio) hoac "videoURL|audioURL"
    // (adaptive video-only + audio, cho mpv --audio-file) hoac "" khi khong
    // co URL truc tiep (cipher/DASH) -> code goi roi xuong yt-dlp cu.
    // quality: "360" (uu tien itag18) hoac "720" (progressive 720, neu khong
    // co thi adaptive video 720 + audio).
    static std::string resolveStream(const std::string& videoId,
                                     const std::string& quality);

    // Mo ta video (videoDetails.shortDescription), cat gon ~600 ky tu.
    // "" khi khong lay duoc. Dung cho OSD mo ta.
    static std::string videoDescription(const std::string& videoId);

    // Video moi nhat cua kenh (RSS, moi nhat truoc, toi da ~15).
    // Dinh dang dong nhu search (id|title|0||0). "" uploader vi RSS khong
    // can — UI hien title la du.
    static std::vector<std::string> channelVideos(const std::string& channelId,
                                                  int maxResults);
};

} // namespace RomCloud
