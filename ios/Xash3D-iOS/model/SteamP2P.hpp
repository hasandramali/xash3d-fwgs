#pragma once
#include "SteamWire.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>

namespace svensteam {
class Stream {
    std::function<void(const Buffer &)> output, input;
    std::deque<Buffer> pending;
    std::map<uint32_t, Buffer> reordered;
    Buffer frames, flight;
    uint32_t receiveNext = 0, sendNext = 1, echoTimestamp = 0, flightSequence = 0;
    bool peerConnected = false, acknowledged = false;
    uint64_t lastSend = 0;
    size_t reorderedBytes = 0;
    void packet(uint32_t sequence, unsigned flags, const Buffer &payload) {
        Buffer result = {1}; integer(result, 0, 4, true); integer(result, sequence, 4, true); integer(result, receiveNext, 4, true);
        result.push_back(0); result.push_back(uint8_t(flags)); integer(result, 61440, 2, true);
        integer(result, milliseconds(), 4, true); integer(result, echoTimestamp, 4, true); append(result, payload); output(result);
    }
    void appendFrames(const Buffer &bytes) {
        if (frames.size() + bytes.size() > 65536) throw std::runtime_error("P2P stream buffer limit");
        append(frames, bytes); size_t offset = 0;
        while (frames.size() - offset >= 4) {
            size_t length = size_t(readInteger(frames, offset, 4, true));
            if (!length || length > 4095) throw std::runtime_error("P2P stream frame length");
            if (frames.size() - offset - 4 < length) break;
            input(Buffer(frames.begin() + offset + 4, frames.begin() + offset + 4 + length)); offset += 4 + length;
        }
        frames.erase(frames.begin(), frames.begin() + offset);
    }
public:
    Stream(std::function<void(const Buffer &)> send, std::function<void(const Buffer &)> receive) : output(send), input(receive) {}
    bool isOpen() const { return peerConnected && acknowledged; }
    void tick(uint64_t now) {
        if (!isOpen()) { if (now - lastSend >= 1000) { packet(0, 2, {0}); lastSend = now; } return; }
        if (flight.empty() && !pending.empty()) { flight = pending.front(); pending.pop_front(); flightSequence = sendNext; sendNext += uint32_t(flight.size()); lastSend = 0; }
        if (!flight.empty() && now - lastSend >= 1000) { packet(flightSequence, 0, flight); lastSend = now; }
    }
    void send(const Buffer &application) {
        if (application.empty() || application.size() > 1205) throw std::runtime_error("P2P application packet size");
        Buffer frame; integer(frame, application.size(), 4, true); append(frame, application);
        if (pending.size() + (frame.size() + 1023) / 1024 > 32) return;
        for (size_t offset = 0; offset < frame.size(); offset += 1024)
            pending.emplace_back(frame.begin() + offset, frame.begin() + std::min(frame.size(), offset + 1024));
    }
    void receive(const Buffer &bytes) {
        if (bytes.size() < 25 || bytes.size() > 8192 || bytes[0] != 1 || readInteger(bytes, 1, 4, true) || bytes[13]) return;
        uint32_t sequence = uint32_t(readInteger(bytes, 5, 4, true)), acknowledgment = uint32_t(readInteger(bytes, 9, 4, true));
        unsigned flags = bytes[14]; if (flags & ~6) return;
        Buffer payload(bytes.begin() + 25, bytes.end());
        if (flags & 4) throw std::runtime_error("Steam P2P stream reset");
        if (flags & 2) {
            if (payload.empty() || payload.size() > 128 || payload[0] || sequence) return;
            if (!peerConnected) { receiveNext = uint32_t(payload.size()); peerConnected = true; }
            else if (receiveNext < payload.size()) return;
        }
        if (acknowledgment >= 1 && acknowledgment <= sendNext && peerConnected) acknowledged = true;
        if (!(flags & 2) && !payload.empty()) {
            if (!isOpen()) return;
            int32_t distance = int32_t(sequence - receiveNext);
            if (!distance) {
                appendFrames(payload); receiveNext += uint32_t(payload.size());
                while (reordered.count(receiveNext)) {
                    Buffer next = reordered[receiveNext]; reordered.erase(receiveNext); reorderedBytes -= next.size();
                    appendFrames(next); receiveNext += uint32_t(next.size());
                }
            } else if (distance > 0 && distance < 61440 && reordered.size() < 32 && reorderedBytes + payload.size() <= 61440 && !reordered.count(sequence)) {
                reordered[sequence] = payload; reorderedBytes += payload.size();
            }
        }
        if (!flight.empty() && acknowledgment == sendNext) flight.clear();
        echoTimestamp = uint32_t(readInteger(bytes, 17, 4, true));
        if (!payload.empty()) packet(sendNext, 0, {});
    }
};
inline bool tokenValid(const std::string &token, size_t min, size_t max) {
    return token.size() >= min && token.size() <= max && token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") == std::string::npos;
}
inline void terminated(Buffer &buffer, const std::string &value) {
    if (value.size() >= 1024 || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid candidate string");
    buffer.insert(buffer.end(), value.begin(), value.end()); buffer.push_back(0);
}
inline std::string terminated(const Buffer &buffer, size_t &offset) {
    size_t start = offset;
    while (offset < buffer.size() && offset - start < 1024) if (!buffer[offset++]) return std::string(buffer.begin() + start, buffer.begin() + offset - 1);
    throw std::runtime_error("Invalid candidate terminator");
}
inline void attribute(Buffer &buffer, unsigned type, const Buffer &value) {
    integer(buffer, type, 2, true); integer(buffer, value.size(), 2, true); append(buffer, value);
    while (buffer.size() % 4) buffer.push_back(0);
}
inline Buffer binding(unsigned type, const Buffer &transaction, const std::string &username, const sockaddr_in *mapped = nullptr) {
    if (transaction.size() != 16 || !tokenValid(username, 2, 32)) throw std::runtime_error("Invalid STUN identity");
    Buffer attributes; attribute(attributes, 6, Buffer(username.begin(), username.end()));
    if (mapped) {
        Buffer address; integer(address, 1, 2, true); integer(address, ntohs(mapped->sin_port), 2, true);
        integer(address, ntohl(mapped->sin_addr.s_addr), 4, true); attribute(attributes, 1, address);
    }
    Buffer result; integer(result, type, 2, true); integer(result, attributes.size(), 2, true); append(result, transaction); append(result, attributes); return result;
}
class P2P {
    struct Path { sockaddr_in address = {}; std::string username; Buffer transaction; uint64_t sent = 0, proven = 0; };
    std::recursive_mutex mutex;
    int socketFD = -1;
    std::atomic<bool> stopped{false};
    std::vector<Path> paths;
    std::deque<Buffer> queued;
    int selected = -1;
    bool ready = false, receivedGame = false;
    uint64_t local, peer, lastConnectivity, streamStarted = 0;
    std::string username, password;
    std::function<void(const Buffer &)> signal, deliver;
    std::function<void(const std::string &)> progress, failure;
    Stream stream;
    static bool same(const sockaddr_in &left, const sockaddr_in &right) { return left.sin_addr.s_addr == right.sin_addr.s_addr && left.sin_port == right.sin_port; }
    static std::string token() {
        const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result; for (unsigned index = 0; index < 16; ++index) result += alphabet[arc4random_uniform(64)]; return result;
    }
    void transmit(const Buffer &data, const sockaddr_in &destination) {
        if (sendto(socketFD, data.data(), data.size(), 0, (const sockaddr *)&destination, sizeof(destination)) != ssize_t(data.size())) throw std::runtime_error("Steam P2P UDP send failed");
    }
    void deliverGame(const Buffer &packet) {
        if (packet.size() > 6 && packet.size() <= 4096 && packet[0] == 0 && packet[1] == 1 && readInteger(packet, 2, 4) == 1) {
            receivedGame = true; deliver(Buffer(packet.begin() + 6, packet.end()));
        }
    }
    void sendGame(const Buffer &packet) {
        if (receivedGame) transmit(packet, paths[selected].address);
        else { stream.send(Buffer(packet.begin() + 1, packet.end())); stream.tick(milliseconds()); }
    }
    void updateReady() {
        if (!ready && selected >= 0 && stream.isOpen()) {
            ready = true; progress("ready");
            while (!queued.empty()) { sendGame(queued.front()); queued.pop_front(); }
        }
    }
    void gather() {
        sockaddr_in bound = {}; socklen_t size = sizeof(bound); getsockname(socketFD, (sockaddr *)&bound, &size);
        ifaddrs *interfaces = nullptr; if (getifaddrs(&interfaces)) throw std::runtime_error("Could not enumerate network interfaces");
        unsigned count = 0;
        try {
            for (ifaddrs *entry = interfaces; entry && count < 16; entry = entry->ifa_next) {
                if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK)) continue;
                auto address = (sockaddr_in *)entry->ifa_addr; char textAddress[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &address->sin_addr, textAddress, sizeof(textAddress));
                Buffer candidate; terminated(candidate, "valvep2p"); terminated(candidate, "udp"); terminated(candidate, textAddress);
                integer(candidate, ntohs(bound.sin_port), 2, true); integer(candidate, 0x3f800000, 4, true);
                terminated(candidate, username); terminated(candidate, password); terminated(candidate, "local"); terminated(candidate, entry->ifa_name);
                integer(candidate, 0, 4, true); candidate.push_back(0);
                Buffer message; fixed(message, 1, peer); fixed(message, 2, local); number(message, 3, 225840); data(message, 4, candidate); number(message, 7, 225840); signal(message); ++count;
            }
        } catch (...) { freeifaddrs(interfaces); throw; }
        freeifaddrs(interfaces);
        if (!count) throw std::runtime_error("No IPv4 interface for Steam P2P");
        progress("gathered local_candidates=" + std::to_string(count));
    }
    void tick() {
        uint64_t now = milliseconds();
        for (auto &path : paths) if (now - path.sent >= 1000) {
            auto transaction = token(); path.transaction.assign(transaction.begin(), transaction.end()); path.sent = now;
            transmit(binding(1, path.transaction, path.username + username), path.address);
        }
        if (selected >= 0 && now - paths[selected].proven >= 10000) { selected = -1; ready = false; progress("rechecking"); }
        if (selected < 0 && now - lastConnectivity > 45000) throw std::runtime_error("Steam P2P ICE timed out");
        if (selected >= 0) stream.tick(now);
        if (selected >= 0 && !stream.isOpen() && streamStarted && now - streamStarted > 45000) throw std::runtime_error("Steam P2P stream handshake timed out");
        updateReady();
    }
    void incoming(const Buffer &packet, const sockaddr_in &source) {
        if (packet.size() >= 20) {
            unsigned type = unsigned(readInteger(packet, 0, 2, true)), size = unsigned(readInteger(packet, 2, 2, true));
            if ((type == 1 || type == 0x101) && size == packet.size() - 20) {
                if (readInteger(packet, 4, 4, true) == 0x2112a442) return;
                Buffer transaction(packet.begin() + 4, packet.begin() + 20); std::string remoteUsername;
                size_t offset = 20;
                while (offset < packet.size()) {
                    if (packet.size() - offset < 4) return;
                    unsigned attr = unsigned(readInteger(packet, offset, 2, true)), length = unsigned(readInteger(packet, offset + 2, 2, true)); offset += 4;
                    size_t padded = (length + 3) & ~3;
                    if (padded > packet.size() - offset) return;
                    if (attr == 6) {
                        if (!remoteUsername.empty()) return;
                        remoteUsername.assign(packet.begin() + offset, packet.begin() + offset + length);
                        if (!tokenValid(remoteUsername, 2, 32)) return;
                    }
                    offset += padded;
                }
                int accepted = -1;
                for (size_t index = 0; index < paths.size(); ++index) {
                    const auto &path = paths[index];
                    if ((type == 1 && remoteUsername == username + path.username) ||
                        (type == 0x101 && same(path.address, source) && path.transaction == transaction && remoteUsername == path.username + username)) { accepted = int(index); break; }
                }
                if (accepted < 0) return;
                if (type == 1) {
                    transmit(binding(0x101, transaction, remoteUsername, &source), source);
                    if (!same(paths[accepted].address, source)) {
                        if (paths.size() >= 32) return;
                        Path learned; learned.address = source; learned.username = paths[accepted].username; paths.push_back(learned); accepted = int(paths.size() - 1);
                    }
                }
                paths[accepted].proven = lastConnectivity = milliseconds();
                if (selected < 0) { selected = accepted; if (!streamStarted) streamStarted = milliseconds(); progress("ice_ready opening_stream"); }
                return;
            }
        }
        bool proven = false;
        for (const auto &path : paths) if (same(path.address, source) && path.proven && milliseconds() - path.proven < 10000) proven = true;
        if (!proven) return;
        if (!packet.empty() && packet[0] == 1) { stream.receive(packet); updateReady(); } else deliverGame(packet);
    }
public:
    P2P(uint64_t localID, uint64_t peerID, std::function<void(const Buffer &)> signaling, std::function<void(const Buffer &)> packets,
        std::function<void(const std::string &)> state, std::function<void(const std::string &)> errors) :
        local(localID), peer(peerID), lastConnectivity(milliseconds()), username(token()), password(token()), signal(signaling), deliver(packets), progress(state), failure(errors),
        stream([this](const Buffer &packet){ if (selected >= 0) transmit(packet, paths[selected].address); }, [this](const Buffer &packet){ deliverGame(packet); }) {
        socketFD = socket(AF_INET, SOCK_DGRAM, 0); if (socketFD < 0) throw std::runtime_error("Could not create Steam P2P socket");
        sockaddr_in address = {}; address.sin_family = AF_INET;
        if (bind(socketFD, (sockaddr *)&address, sizeof(address))) { ::close(socketFD); socketFD = -1; throw std::runtime_error("Could not bind Steam P2P socket"); }
    }
    ~P2P() { close(); }
    void close() { std::lock_guard<std::recursive_mutex> lock(mutex); stopped = true; if (socketFD >= 0) { ::close(socketFD); socketFD = -1; } queued.clear(); }
    void receiveSignal(int message, const Buffer &body) {
        std::lock_guard<std::recursive_mutex> lock(mutex); if (stopped) return;
        if (body.size() > 16384) return;
        uint64_t destination = value(body, 1);
        if ((destination && destination != local) || value(body, 2) != peer || value(body, 3) != 225840 || (value(body, 7) && value(body, 7) != 225840)) return;
        if (message == 5435) throw std::runtime_error("Steam peer rejected the P2P session");
        Buffer candidate = blob(body, 4); if (candidate.empty() || candidate.size() > 7179 || paths.size() >= 32) return;
        size_t offset = 0; terminated(candidate, offset); auto protocol = terminated(candidate, offset), address = terminated(candidate, offset);
        unsigned port = unsigned(readInteger(candidate, offset, 2, true)); offset += 6;
        std::string fragment = terminated(candidate, offset); terminated(candidate, offset); terminated(candidate, offset); terminated(candidate, offset);
        readInteger(candidate, offset, 4, true); offset += 4;
        if (candidate.size() - offset > 1 || protocol != "udp" || !port || !tokenValid(fragment, 1, 16)) return;
        Path path; path.address.sin_family = AF_INET; path.address.sin_port = htons(port); path.username = fragment;
        if (inet_pton(AF_INET, address.c_str(), &path.address.sin_addr) != 1) return;
        uint32_t ip = ntohl(path.address.sin_addr.s_addr); if (ip >> 24 == 0 || ip >> 24 == 127 || ip >> 24 >= 224) return;
        for (const auto &existing : paths) if (same(existing.address, path.address) && existing.username == path.username) return;
        paths.push_back(path); progress("checking remote_candidates=" + std::to_string(paths.size()));
    }
    void send(const Buffer &payload) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (stopped || payload.empty() || payload.size() > 1200) return;
        Buffer packet = {0, 0}; append(packet, payload);
        if (ready && selected >= 0 && milliseconds() - paths[selected].proven < 10000) sendGame(packet);
        else if (queued.size() < 32) queued.push_back(packet);
    }
    void run() {
        try {
            { std::lock_guard<std::recursive_mutex> lock(mutex); if (stopped) return; gather(); }
            while (!stopped) {
                int descriptor;
                { std::lock_guard<std::recursive_mutex> lock(mutex); if (stopped) break; tick(); descriptor = socketFD; }
                pollfd pending = {descriptor, POLLIN, 0}; if (poll(&pending, 1, 100) <= 0) continue;
                std::lock_guard<std::recursive_mutex> lock(mutex); if (stopped) break;
                Buffer packet(8192); sockaddr_in source = {}; socklen_t length = sizeof(source);
                ssize_t count = recvfrom(socketFD, packet.data(), packet.size(), 0, (sockaddr *)&source, &length);
                if (count <= 0 || count == ssize_t(packet.size())) continue;
                packet.resize(size_t(count)); incoming(packet, source);
            }
        } catch (const std::exception &error) { if (!stopped) { close(); failure(error.what()); } }
        close();
    }
};
}
