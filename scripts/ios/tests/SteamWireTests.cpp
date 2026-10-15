#include "../../../ios/Xash3D-iOS/model/SteamP2P.hpp"
#include "../../../ios/Xash3D-iOS/model/SteamContent.hpp"
#include <cassert>
#include <iostream>
#include <random>

using namespace svensteam;

static void protobuf() {
    Buffer data; number(data, 1, UINT64_MAX); fixed(data, 2, 0x1122334455667788ULL); text(data, 3, "Steam");
    assert(value(data, 1) == UINT64_MAX && value(data, 2) == 0x1122334455667788ULL && string(data, 3) == "Steam");
    bool rejected = false;
    try { fields({10, 255, 127}); } catch (const std::exception &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { fields({8, 255, 255, 255, 255, 255, 255, 255, 255, 255, 2}); } catch (const std::exception &) { rejected = true; }
    assert(rejected);
    auto requestHeader = header(76561198000000001ULL, 123, 456, "Authentication.PollAuthSessionStatus#1");
    assert(value(requestHeader, 1) == 76561198000000001ULL && value(requestHeader, 2) == 123 && value(requestHeader, 10) == 456);
    assert(string(requestHeader, 12) == "Authentication.PollAuthSessionStatus#1");
}
static void masterlist() {
    Buffer defaults; number(defaults, 2, 27015);
    Buffer entry; Buffer address; fixed(address, 1, 0xc0000201, 4); data(entry, 4, address);
    Buffer response; data(response, 1, entry); data(response, 1, entry); data(response, 3, defaults);
    auto addresses = servers(response); assert(addresses.size() == 1 && addresses[0] == "192.0.2.1:27015");
    Buffer bad; number(bad, 1, 0x7f000001); number(bad, 2, 27015); data(response, 1, bad);
    assert(servers(response).size() == 1);
}
static void broker() {
    auto reply = connectReply(0x12345678, 99, 76561198000000001ULL, {10, 20, 30});
    assert(std::string(reply.begin(), reply.begin() + 12) == "sb_connect2\n");
    assert(readInteger(reply, 12, 4) == 0x12345678 && readInteger(reply, 16, 4) == 99);
    assert(readInteger(reply, 20, 8) == 76561198000000001ULL && readInteger(reply, 28, 4) == 3);
    auto frame = brokerFrame(reply);
    assert(frame.size() == reply.size() + 6 && readInteger(frame, 4, 2) == reply.size());
    auto legacy = connectReply(123, 0, 1, {2});
    assert(std::string(legacy.begin(), legacy.begin() + 11) == "sb_connect\n" && readInteger(legacy, 15, 8) == 1);
}
static void stun() {
    Buffer transaction; for (unsigned index = 0; index < 16; ++index) transaction.push_back(uint8_t(index));
    auto request = binding(1, transaction, "abcdefghijklmnop");
    assert(request.size() == 40 && readInteger(request, 0, 2, true) == 1 && readInteger(request, 2, 2, true) == 20);
    assert(readInteger(request, 20, 2, true) == 6 && readInteger(request, 22, 2, true) == 16);
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_port = htons(27015); inet_pton(AF_INET, "192.0.2.1", &address.sin_addr);
    auto reply = binding(0x101, transaction, "abcdefghijklmnop", &address);
    assert(reply.size() == 52 && readInteger(reply, 44, 2, true) == 1 && readInteger(reply, 46, 2, true) == 27015);
    assert(readInteger(reply, 48, 4, true) == 0xc0000201);
}
static void streams() {
    std::deque<Buffer> toLeft, toRight; std::vector<Buffer> received;
    Stream left([&](const Buffer &packet){ toRight.push_back(packet); }, [](const Buffer &){});
    Stream right([&](const Buffer &packet){ toLeft.push_back(packet); }, [&](const Buffer &packet){ received.push_back(packet); });
    auto pump = [&]{
        for (unsigned round = 0; round < 20 && (!toLeft.empty() || !toRight.empty()); ++round) {
            while (!toLeft.empty()) { auto packet = toLeft.front(); toLeft.pop_front(); left.receive(packet); }
            while (!toRight.empty()) { auto packet = toRight.front(); toRight.pop_front(); right.receive(packet); }
        }
    };
    left.tick(milliseconds()); right.tick(milliseconds()); pump(); assert(left.isOpen() && right.isOpen());
    Buffer game(1200); for (size_t index = 0; index < game.size(); ++index) game[index] = uint8_t(index);
    left.send(game);
    left.tick(milliseconds() + 1000); toRight.clear();
    left.tick(milliseconds() + 2001); pump();
    left.tick(milliseconds() + 3002); pump();
    assert(received.size() == 1 && received[0] == game);
    bool rejected = false; try { left.send(Buffer(1206)); } catch (const std::exception &) { rejected = true; } assert(rejected);
}
static void malformed() {
    std::mt19937 random(12345);
    Stream stream([](const Buffer &){}, [](const Buffer &){});
    for (unsigned count = 0; count < 5000; ++count) {
        Buffer data(random() % 128); for (auto &byte : data) byte = uint8_t(random());
        try { fields(data); } catch (const std::exception &) {}
        try { stream.receive(data); } catch (const std::exception &) {}
    }
}
static void contentDownload() {
    assert(clientHeartbeat == 703);
    Buffer responseHeader; fixed(responseHeader, 11, 42);
    assert(contentReplyMatches(147, responseHeader, 147, 42));
    assert(!contentReplyMatches(147, responseHeader, 147, 43));
    assert(!contentReplyMatches(703, responseHeader, 147, 42));
    assert(!contentReplyMatches(147, {}, 147, 42));

    Buffer key(32, 0xa5), response;
    number(response, 1, 1); number(response, 2, 225841); data(response, 3, key);
    assert(depotDecryptionKey(response, 225841) == key);
    bool rejected = false;
    try { depotDecryptionKey(response, 1); } catch (const std::exception &) { rejected = true; }
    assert(rejected);
    rejected = false;
    Buffer denied; number(denied, 1, 5); number(denied, 2, 225841); data(denied, 3, key);
    try { depotDecryptionKey(denied, 225841); } catch (const std::exception &) { rejected = true; }
    assert(rejected);
    rejected = false;
    Buffer invalid; number(invalid, 1, 1); number(invalid, 2, 225841); data(invalid, 3, {1, 2});
    try { depotDecryptionKey(invalid, 225841); } catch (const std::exception &) { rejected = true; }
    assert(rejected);

    Buffer cdn, entry; text(entry, 8, "cache.example.com"); text(entry, 9, "cdn.example.com"); data(cdn, 1, entry); data(cdn, 1, entry);
    Buffer proxy; text(proxy, 8, "proxy.example.com"); number(proxy, 10, 1); data(cdn, 1, proxy);
    Buffer unsafe; text(unsafe, 9, "example.com/path"); data(cdn, 1, unsafe);
    assert(contentHosts(cdn) == std::vector<std::string>{"cdn.example.com"});
    for (const auto &path : {"client.dll", "libserver.SO", "nested/lib.a", "setup.exe", "run.BAT"}) assert(foreignGameBinary(path));
    for (const auto &path : {"models/a.mdl", "sound/a.wav", "sprites/dollar.spr", "dll-folder/map.bsp", "maps/a.txt"}) assert(!foreignGameBinary(path));
}
int main() {
    protobuf(); masterlist(); broker(); stun(); streams(); malformed(); contentDownload();
    std::cout << "PASS: protobuf bounds, CM headers, masterlist, legacy STUN, stream handshake/retransmission, malformed input, content responses and binary filtering\n";
}
