#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <cctype>

namespace RomCloud {

class JsonHelper {
public:
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
                if (c == 'n') result += '\n';
                else if (c == 'r') result += '\r';
                else if (c == 't') result += '\t';
                else if (c == '\"') result += '\"';
                else if (c == '\\') result += '\\';
                else result += c;
                escaped = false;
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
