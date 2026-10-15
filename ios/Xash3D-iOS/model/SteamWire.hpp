#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <functional>
#include <chrono>

namespace svensteam {
using Buffer = std::vector<uint8_t>;
inline uint64_t milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void append(Buffer &dest, const Buffer &source) { dest.insert(dest.end(), source.begin(), source.end()); }
inline void integer(Buffer &dest, uint64_t value, unsigned count, bool big = false) {
    for (unsigned index = 0; index < count; ++index) dest.push_back(uint8_t(value >> (8 * (big ? count - index - 1 : index))));
}
inline uint64_t readInteger(const Buffer &source, size_t offset, unsigned count, bool big = false) {
    if (offset > source.size() || count > source.size() - offset || count > 8) throw std::runtime_error("Truncated integer");
    uint64_t value = 0;
    for (unsigned index = 0; index < count; ++index) value |= uint64_t(source[offset + index]) << (8 * (big ? count - index - 1 : index));
    return value;
}
inline void varint(Buffer &dest, uint64_t value) {
    while (value >= 128) { dest.push_back(uint8_t(value) | 128); value >>= 7; }
    dest.push_back(uint8_t(value));
}
inline void number(Buffer &dest, unsigned field, uint64_t value) { varint(dest, field << 3); varint(dest, value); }
inline void fixed(Buffer &dest, unsigned field, uint64_t value, unsigned size = 8) {
    varint(dest, (field << 3) | (size == 8 ? 1 : 5)); integer(dest, value, size);
}
inline void data(Buffer &dest, unsigned field, const Buffer &value) {
    varint(dest, (field << 3) | 2); varint(dest, value.size()); append(dest, value);
}
inline void text(Buffer &dest, unsigned field, const std::string &value) { data(dest, field, Buffer(value.begin(), value.end())); }
struct Field { unsigned id, wire; uint64_t value = 0; Buffer bytes; };
class Reader {
    const Buffer &buffer;
    size_t offset = 0;
    uint64_t readVarint() {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (offset == buffer.size()) throw std::runtime_error("Truncated protobuf");
            uint8_t next = buffer[offset++];
            if (shift == 63 && next > 1) throw std::runtime_error("Protobuf integer overflow");
            value |= uint64_t(next & 127) << shift;
            if (!(next & 128)) return value;
        }
        throw std::runtime_error("Invalid protobuf integer");
    }
public:
    explicit Reader(const Buffer &source) : buffer(source) {}
    bool next(Field &field) {
        if (offset == buffer.size()) return false;
        uint64_t tag = readVarint();
        if (!(tag >> 3) || tag >> 3 > 0x1fffffff) throw std::runtime_error("Invalid protobuf tag");
        field = {}; field.id = unsigned(tag >> 3); field.wire = tag & 7;
        if (field.wire == 0) field.value = readVarint();
        else if (field.wire == 1 || field.wire == 5) {
            unsigned length = field.wire == 1 ? 8 : 4;
            field.value = readInteger(buffer, offset, length); offset += length;
        } else if (field.wire == 2) {
            uint64_t length = readVarint();
            if (length > buffer.size() - offset) throw std::runtime_error("Truncated protobuf bytes");
            field.bytes.assign(buffer.begin() + offset, buffer.begin() + offset + size_t(length)); offset += size_t(length);
        } else throw std::runtime_error("Unsupported protobuf wire type");
        return true;
    }
};
inline std::vector<Field> fields(const Buffer &buffer) {
    Reader reader(buffer); Field field; std::vector<Field> result;
    while (reader.next(field)) result.push_back(field);
    return result;
}
inline uint64_t value(const Buffer &buffer, unsigned id, uint64_t fallback = 0) {
    for (const auto &field : fields(buffer)) if (field.id == id && field.wire != 2) return field.value;
    return fallback;
}
inline Buffer blob(const Buffer &buffer, unsigned id) {
    for (const auto &field : fields(buffer)) if (field.id == id && field.wire == 2) return field.bytes;
    return {};
}
inline std::string string(const Buffer &buffer, unsigned id) {
    auto bytes = blob(buffer, id); return std::string(bytes.begin(), bytes.end());
}
inline Buffer header(uint64_t steamID, int session, uint64_t job = 0, const std::string &method = "") {
    Buffer result; fixed(result, 1, steamID); number(result, 2, uint32_t(session));
    if (job) fixed(result, 10, job);
    if (!method.empty()) text(result, 12, method);
    return result;
}
inline Buffer brokerFrame(const Buffer &payload) {
    if (payload.empty() || payload.size() > 65535) throw std::runtime_error("Broker payload size");
    Buffer result = {'S', 'B', 'R', 'K'}; integer(result, payload.size(), 2); append(result, payload); return result;
}
inline Buffer connectReply(uint32_t challenge, uint32_t request, uint64_t steamID, const Buffer &ticket) {
    std::string prefix = request ? "sb_connect2\n" : "sb_connect\n";
    Buffer result(prefix.begin(), prefix.end()); integer(result, challenge, 4);
    if (request) integer(result, request, 4);
    integer(result, steamID, 8); integer(result, ticket.size(), 4); append(result, ticket); return result;
}
struct Endpoint { uint32_t ip = 0; unsigned query = 0, game = 0; };
inline Endpoint endpoint(const Buffer &buffer) {
    Endpoint result;
    for (const auto &field : fields(buffer)) {
        if (field.id == 1 && field.wire == 0 && !result.ip) result.ip = uint32_t(field.value);
        if (field.id == 2 && field.wire == 0) result.query = unsigned(field.value);
        if (field.id == 9 && field.wire == 0) result.game = unsigned(field.value);
        if (field.id == 4 && field.wire == 2) result.ip = uint32_t(value(field.bytes, 1));
    }
    return result;
}
inline std::vector<std::string> servers(const Buffer &buffer) {
    if (buffer.size() > 4 * 1024 * 1024) throw std::runtime_error("Masterlist too large");
    Endpoint defaults; std::vector<Endpoint> entries; std::vector<std::string> result;
    for (const auto &field : fields(buffer)) {
        if (field.id == 1 && field.wire == 2) {
            if (entries.size() >= 5000) throw std::runtime_error("Too many masterlist entries");
            entries.push_back(endpoint(field.bytes));
        } else if (field.id == 3 && field.wire == 2) defaults = endpoint(field.bytes);
        else if (field.id == 2 && !field.bytes.empty()) throw std::runtime_error(std::string(field.bytes.begin(), field.bytes.end()));
    }
    std::map<std::string, bool> seen;
    for (const auto &entry : entries) {
        uint32_t ip = entry.ip ? entry.ip : defaults.ip;
        unsigned port = entry.query ? entry.query : defaults.query ? defaults.query : entry.game ? entry.game : defaults.game;
        if (!ip || ip >= 0xe0000000 || ip >> 24 == 127 || !port || port > 65535) continue;
        std::string address = std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
            std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255) + ":" + std::to_string(port);
        if (seen.emplace(address, true).second) result.push_back(address);
    }
    return result;
}
}
