#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace RomCloud {

class TelexHelper {
public:
    struct VowelInfo {
        std::string base;
        int tone; // 0: none, 1: sac, 2: huyen, 3: hoi, 4: nga, 5: nang
        bool isUpper;
    };

    static void popUtf8(std::string& s) {
        if (s.empty()) return;
        while (!s.empty()) {
            unsigned char c = s.back();
            s.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
    }

    static std::vector<std::string> splitUtf8(const std::string& str) {
        std::vector<std::string> chars;
        size_t i = 0;
        while (i < str.length()) {
            unsigned char c = str[i];
            size_t len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            if (i + len > str.length()) len = str.length() - i;
            chars.push_back(str.substr(i, len));
            i += len;
        }
        return chars;
    }

    static const std::vector<std::vector<std::string>>& getTable() {
        static const std::vector<std::vector<std::string>> table = {
            {"a", "á", "à", "ả", "ã", "ạ"},
            {"ă", "ắ", "ằ", "ẳ", "ẵ", "ặ"},
            {"â", "ấ", "ầ", "ẩ", "ẫ", "ậ"},
            {"e", "é", "è", "ẻ", "ẽ", "ẹ"},
            {"ê", "ế", "ề", "ể", "ễ", "ệ"},
            {"i", "í", "ì", "ỉ", "ĩ", "ị"},
            {"o", "ó", "ò", "ỏ", "õ", "ọ"},
            {"ô", "ố", "ồ", "ổ", "ỗ", "ộ"},
            {"ơ", "ớ", "ờ", "ở", "ỡ", "ợ"},
            {"u", "ú", "ù", "ủ", "ũ", "ụ"},
            {"ư", "ứ", "ừ", "ử", "ữ", "ự"},
            {"y", "ý", "ỳ", "ỷ", "ỹ", "ỵ"},
            {"A", "Á", "À", "Ả", "Ã", "Ạ"},
            {"Ă", "Ắ", "Ằ", "Ẳ", "Ẵ", "Ặ"},
            {"Â", "Ấ", "Ầ", "Ẩ", "Ẫ", "Ậ"},
            {"E", "É", "È", "Ẻ", "Ẽ", "Ẹ"},
            {"Ê", "Ế", "Ề", "Ể", "Ễ", "Ệ"},
            {"I", "Í", "Ì", "Ỉ", "Ĩ", "Ị"},
            {"O", "Ó", "Ò", "Ỏ", "Õ", "Ọ"},
            {"Ô", "Ố", "Ồ", "Ổ", "Ỗ", "Ộ"},
            {"Ơ", "Ớ", "Ờ", "Ở", "Ỡ", "Ợ"},
            {"U", "Ú", "Ù", "Ủ", "Ũ", "Ụ"},
            {"Ư", "Ứ", "Ừ", "Ử", "Ữ", "Ự"},
            {"Y", "Ý", "Ỳ", "Ỷ", "Ỹ", "Ỵ"}
        };
        return table;
    }

    static bool getVowelInfo(const std::string& ch, VowelInfo& info) {
        const auto& table = getTable();
        for (const auto& row : table) {
            for (int t = 0; t < 6; ++t) {
                if (row[t] == ch) {
                    info.base = row[0];
                    info.tone = t;
                    info.isUpper = (row[0][0] >= 'A' && row[0][0] <= 'Z');
                    return true;
                }
            }
        }
        return false;
    }

    static bool isVowel(const std::string& ch) {
        VowelInfo info;
        return getVowelInfo(ch, info);
    }

    static std::string makeVowel(const std::string& base, int tone) {
        if (tone < 0 || tone > 5) tone = 0;
        const auto& table = getTable();
        for (const auto& row : table) {
            if (row[0] == base) {
                return row[tone];
            }
        }
        return base;
    }

    static std::string processTelex(const std::string& text, char inputChar) {
        if (inputChar == ' ') {
            return text + " ";
        }

        // Split text into prefix and last word
        size_t lastSpace = text.find_last_of(" \t\n");
        std::string prefix = (lastSpace != std::string::npos) ? text.substr(0, lastSpace + 1) : "";
        std::string wordStr = (lastSpace != std::string::npos) ? text.substr(lastSpace + 1) : text;

        if (wordStr.empty()) {
            return text + std::string(1, inputChar);
        }

        auto chars = splitUtf8(wordStr);
        char lowerKey = std::tolower(inputChar);

        // Rule of Immediate Lock (Chốt Âm):
        // Check if the last character in the active word is an active vowel
        bool lastIsVowel = !chars.empty() && isVowel(chars.back());

        // 1. Double consonants: 'd' + 'd' -> 'đ', 'đ' + 'd' -> 'dd'
        if (lowerKey == 'd') {
            if (!chars.empty()) {
                if (chars.back() == "d" || chars.back() == "D") {
                    bool isUp = (chars.back() == "D");
                    chars.back() = isUp ? "Đ" : "đ";
                    std::string result = prefix;
                    for (const auto& c : chars) result += c;
                    return result;
                } else if (chars.back() == "đ" || chars.back() == "Đ") {
                    bool isUp = (chars.back() == "Đ");
                    chars.back() = isUp ? "D" : "d";
                    chars.push_back(isUp ? "D" : "d");
                    std::string result = prefix;
                    for (const auto& c : chars) result += c;
                    return result;
                }
            }
            return text + std::string(1, inputChar);
        }

        // 2. Vowel transformations: a, e, o, w (ONLY when immediate predecessor is active vowel!)
        if (lastIsVowel && (lowerKey == 'a' || lowerKey == 'e' || lowerKey == 'o' || lowerKey == 'w')) {
            int lastIdx = static_cast<int>(chars.size()) - 1;
            VowelInfo vi;
            if (getVowelInfo(chars[lastIdx], vi)) {
                std::string bLower = vi.base;
                if (bLower == "A") bLower = "a";
                else if (bLower == "Ă") bLower = "ă";
                else if (bLower == "Â") bLower = "â";
                else if (bLower == "E") bLower = "e";
                else if (bLower == "Ê") bLower = "ê";
                else if (bLower == "O") bLower = "o";
                else if (bLower == "Ô") bLower = "ô";
                else if (bLower == "Ơ") bLower = "ơ";
                else if (bLower == "U") bLower = "u";
                else if (bLower == "Ư") bLower = "ư";

                // Revert check (3rd press undo)
                if (lowerKey == 'a' && bLower == "â") {
                    chars[lastIdx] = makeVowel(vi.isUpper ? "A" : "a", vi.tone);
                    chars.push_back(vi.isUpper ? "A" : "a");
                    std::string res = prefix;
                    for (const auto& c : chars) res += c;
                    return res;
                }
                if (lowerKey == 'e' && bLower == "ê") {
                    chars[lastIdx] = makeVowel(vi.isUpper ? "E" : "e", vi.tone);
                    chars.push_back(vi.isUpper ? "E" : "e");
                    std::string res = prefix;
                    for (const auto& c : chars) res += c;
                    return res;
                }
                if (lowerKey == 'o' && bLower == "ô") {
                    chars[lastIdx] = makeVowel(vi.isUpper ? "O" : "o", vi.tone);
                    chars.push_back(vi.isUpper ? "O" : "o");
                    std::string res = prefix;
                    for (const auto& c : chars) res += c;
                    return res;
                }
                if (lowerKey == 'w' && (bLower == "ă" || bLower == "ơ" || bLower == "ư")) {
                    std::string origBase = (bLower == "ă") ? "a" : (bLower == "ơ") ? "o" : "u";
                    chars[lastIdx] = makeVowel(vi.isUpper ? (origBase == "a" ? "A" : origBase == "o" ? "O" : "U") : origBase, vi.tone);
                    chars.push_back(vi.isUpper ? "W" : "w");
                    std::string res = prefix;
                    for (const auto& c : chars) res += c;
                    return res;
                }

                // Transform
                std::string newBase;
                if (lowerKey == 'a' && bLower == "a") newBase = vi.isUpper ? "Â" : "â";
                else if (lowerKey == 'e' && bLower == "e") newBase = vi.isUpper ? "Ê" : "ê";
                else if (lowerKey == 'o' && bLower == "o") newBase = vi.isUpper ? "Ô" : "ô";
                else if (lowerKey == 'w') {
                    if (bLower == "a") newBase = vi.isUpper ? "Ă" : "ă";
                    else if (bLower == "o") newBase = vi.isUpper ? "Ơ" : "ơ";
                    else if (bLower == "u") newBase = vi.isUpper ? "Ư" : "ư";
                }

                if (!newBase.empty()) {
                    chars[lastIdx] = makeVowel(newBase, vi.tone);
                    std::string res = prefix;
                    for (const auto& c : chars) res += c;
                    return res;
                }
            }
        }

        // 3. Tone marks: s (1), f (2), r (3), x (4), j (5), z (0)
        // STRICT LOCK: Tone marks ONLY apply if the immediate last character is an active vowel!
        int targetTone = -1;
        if (lowerKey == 's') targetTone = 1;
        else if (lowerKey == 'f') targetTone = 2;
        else if (lowerKey == 'r') targetTone = 3;
        else if (lowerKey == 'x') targetTone = 4;
        else if (lowerKey == 'j') targetTone = 5;
        else if (lowerKey == 'z') targetTone = 0;

        if (targetTone != -1 && lastIsVowel) {
            int lastIdx = static_cast<int>(chars.size()) - 1;
            VowelInfo vi;
            if (getVowelInfo(chars[lastIdx], vi)) {
                // If same tone already present, toggle off (undo)
                if (vi.tone == targetTone) {
                    targetTone = 0;
                }
                chars[lastIdx] = makeVowel(vi.base, targetTone);
                std::string res = prefix;
                for (const auto& c : chars) res += c;
                return res;
            }
        }

        // If not a vowel transformation and not an active vowel tone mark, append literal
        return text + std::string(1, inputChar);
    }
};

} // namespace RomCloud
