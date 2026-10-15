#pragma once
#include "SteamWire.hpp"
#include <algorithm>
#include <cctype>

namespace svensteam {
constexpr int clientHeartbeat = 703;

inline bool contentReplyMatches(int message, const Buffer &protoHeader, int expected, uint64_t job) {
    if (message != expected) return false;
    uint64_t target = value(protoHeader, 11, UINT64_MAX);
    return !job || target == job;
}

inline Buffer depotDecryptionKey(const Buffer &response, unsigned depot) {
    if (value(response, 1, 2) != 1 || value(response, 2) != depot)
        throw std::runtime_error("Steam denied access to the game content depot");
    Buffer key = blob(response, 3);
    if (key.size() != 32) throw std::runtime_error("Steam returned an invalid depot key");
    return key;
}

inline std::vector<std::string> contentHosts(const Buffer &response) {
    std::vector<std::string> result;
    for (const auto &field : fields(response)) {
        if (field.id != 1 || field.wire != 2) continue;
        if (value(field.bytes, 10)) continue;
        std::string host = string(field.bytes, 9);
        if (host.empty()) host = string(field.bytes, 8);
        if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.' ||
            host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") != std::string::npos)
            continue;
        if (std::find(result.begin(), result.end(), host) == result.end()) result.push_back(host);
        if (result.size() == 20) break;
    }
    return result;
}

inline bool foreignGameBinary(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return char(std::tolower(character));
    });
    return extension == "dll" || extension == "so" || extension == "a" || extension == "exe" || extension == "bat";
}
}
