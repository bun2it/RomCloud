#pragma once

#include <string>
#include <vector>

namespace RomCloud {

// YoutubeInnertube: tìm kiếm YouTube qua API ngầm innertube (POST
// youtubei/v1/search), không cần yt-dlp/python. Nhanh (~2-3s, chỉ tốn
// 1 request HTTPS) để hiện kết quả trước; thất bại -> trả về rỗng để
// code gọi rớt xuống đường yt-dlp cũ.
//
// Mỗi dòng kết quả: "id|title|durSec|uploader|views" (khớp định dạng
// youtube_search.sh hiện tại; '|' trong text đã thay bằng khoảng trắng).
class YoutubeInnertube {
public:
    // query thuong. Tien to dac biet: "feed:<cat>" -> trending (browse
    // FEtrending hoặc fallback search như backend cũ); "channel:<id>" ->
    // rong (de duong cu .py xu ly channel).
    static std::vector<std::string> search(const std::string& query,
                                           int maxResults,
                                           const std::string& params = "");

    static std::vector<std::string> trending(const std::string& categoryId,
                                             int maxResults);

    // Resolve stream thẳng qua player API (không cần yt-dlp).
    // Trả về "videoURL" (progressive, có sẵn audio) hoặc "videoURL|audioURL"
    // (adaptive video-only + audio, cho mpv --audio-file) hoặc "" khi không
    // có URL trực tiếp (cipher/DASH) -> code gọi rớt xuống yt-dlp cũ.
    // quality: "360" (ưu tiên itag18) hoặc "720" (progressive 720, nếu không
    // co thi adaptive video 720 + audio).
    static std::string resolveStream(const std::string& videoId,
                                     const std::string& quality);

    // Mô tả video (videoDetails.shortDescription), cắt gọn ~600 ký tự.
    // "" khi không lấy được. Dùng cho OSD mô tả.
    static std::string videoDescription(const std::string& videoId);

    // Video mới nhất của kênh (RSS, mới nhất trước, tối đa ~15).
    // Định dạng dòng như search (id|title|0||0). "" uploader vì RSS không
    // can — UI hien title la du.
    static std::vector<std::string> channelVideos(const std::string& channelId,
                                                  int maxResults);
};

} // namespace RomCloud
