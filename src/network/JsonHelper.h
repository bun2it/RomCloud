#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <cctype>

namespace RomCloud {

class JsonHelper {
public:
    static void appendUtf8CodePoint(std::string& out, uint32_t cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return; // Surrogate pairs
        if (cp > 0xFFFF) return; // 4-byte emoji codepoints
        // Filter miscellaneous symbol emojis that cause square box glyphs
        if ((cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2300 && cp <= 0x23FF)) return;

        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    static std::string extractString(const std::string& json, const std::string& key) {
        std::string pattern = "\"" + key + "\"";
        size_t pos = json.find(pattern);
        if (pos == std::string::npos) return "";

        size_t colon = json.find(':', pos + pattern.length());
        if (colon == std::string::npos) return "";

        size_t quoteStart = json.find('\"', colon + 1);
        if (quoteStart == std::string::npos) return "";

        std::string result;
        bool escaped = false;
        for (size_t i = quoteStart + 1; i < json.length(); ++i) {
            char c = json[i];
            if (escaped) {
                if (c == 'n') {
                    result += '\n';
                    escaped = false;
                } else if (c == 'r') {
                    result += '\r';
                    escaped = false;
                } else if (c == 't') {
                    result += '\t';
                    escaped = false;
                } else if (c == '\"') {
                    result += '\"';
                    escaped = false;
                } else if (c == '\\') {
                    result += '\\';
                    escaped = false;
                } else if (c == 'u' && i + 4 < json.length()) {
                    try {
                        std::string hexStr = json.substr(i + 1, 4);
                        size_t idx = 0;
                        uint32_t cp = std::stoul(hexStr, &idx, 16);
                        if (idx == 4) {
                            i += 4;
                            // Check for surrogate pair \uD8xx\uDCxx
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < json.length() && json[i + 1] == '\\' && json[i + 2] == 'u') {
                                std::string lowHex = json.substr(i + 3, 4);
                                size_t lidx = 0;
                                uint32_t lowCp = std::stoul(lowHex, &lidx, 16);
                                if (lidx == 4 && lowCp >= 0xDC00 && lowCp <= 0xDFFF) {
                                    i += 6;
                                    escaped = false;
                                    continue;
                                }
                            }
                            appendUtf8CodePoint(result, cp);
                            escaped = false;
                            continue;
                        }
                    } catch (...) {}
                    result += c;
                    escaped = false;
                } else {
                    result += c;
                    escaped = false;
                }
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '\"') {
                break;
            } else {
                result += c;
            }
        }
        return result;
    }

    static int extractInt(const std::string& json, const std::string& key, int defaultVal = 0) {
        std::string pattern = "\"" + key + "\"";
        size_t pos = json.find(pattern);
        if (pos == std::string::npos) return defaultVal;

        size_t colon = json.find(':', pos + pattern.length());
        if (colon == std::string::npos) return defaultVal;

        size_t start = colon + 1;
        while (start < json.length() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n' || json[start] == '\"')) {
            start++;
        }

        size_t end = start;
        while (end < json.length() && (std::isdigit(json[end]) || json[end] == '-')) {
            end++;
        }

        if (start < end) {
            try {
                return std::stoi(json.substr(start, end - start));
            } catch (...) {
                return defaultVal;
            }
        }
        return defaultVal;
    }

    static uint64_t extractUInt64(const std::string& json, const std::string& key, uint64_t defaultVal = 0) {
        // First try as string (Drive API often returns size as "size": "1048576")
        std::string strVal = extractString(json, key);
        if (!strVal.empty()) {
            try {
                return std::stoull(strVal);
            } catch (...) {}
        }

        // Try as raw number
        std::string pattern = "\"" + key + "\"";
        size_t pos = json.find(pattern);
        if (pos == std::string::npos) return defaultVal;

        size_t colon = json.find(':', pos + pattern.length());
        if (colon == std::string::npos) return defaultVal;

        size_t start = colon + 1;
        while (start < json.length() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n' || json[start] == '\"')) {
            start++;
        }

        size_t end = start;
        while (end < json.length() && std::isdigit(json[end])) {
            end++;
        }

        if (start < end) {
            try {
                return std::stoull(json.substr(start, end - start));
            } catch (...) {
                return defaultVal;
            }
        }
        return defaultVal;
    }

    static std::vector<std::string> extractArrayObjects(const std::string& json, const std::string& arrayKey) {
        std::vector<std::string> objects;
        std::string pattern = "\"" + arrayKey + "\"";
        size_t pos = json.find(pattern);
        if (pos == std::string::npos) return objects;

        size_t bracketOpen = json.find('[', pos + pattern.length());
        if (bracketOpen == std::string::npos) return objects;

        bool inString = false;
        bool escaped = false;
        int depth = 0;
        size_t objStart = 0;

        for (size_t i = bracketOpen + 1; i < json.length(); ++i) {
            char c = json[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '\"') {
                inString = !inString;
                continue;
            }
            if (inString) continue;

            if (c == ']') {
                if (depth == 0) break;
            } else if (c == '{') {
                if (depth == 0) {
                    objStart = i;
                }
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    objects.push_back(json.substr(objStart, i - objStart + 1));
                }
            }
        }
        return objects;
    }
};

} // namespace RomCloud
