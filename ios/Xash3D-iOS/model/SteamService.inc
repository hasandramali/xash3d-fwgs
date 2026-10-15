#include <memory>
#include <set>
#include <netinet/in.h>
#include <sstream>
#include <cmath>
#include "SteamP2P.hpp"
#include "SteamContent.hpp"

NSNotificationName const SvenSteamStateChanged = @"SvenSteamStateChanged";

namespace {
using namespace svensteam;
struct SteamReply { int message = 0; Buffer body, header; };
struct SteamPending {
    std::condition_variable changed;
    int expected = 0;
    bool done = false;
    SteamReply reply;
};
static NSDictionary *SteamCredentials() {
    NSDictionary *query = @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: @"su.xash.svenclient.steam", (__bridge id)kSecAttrAccount: @"session",
        (__bridge id)kSecReturnData: @YES};
    CFTypeRef result = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess) return @{};
    NSData *encoded = CFBridgingRelease(result);
    id decoded = [NSJSONSerialization JSONObjectWithData:encoded options:0 error:nil];
    return [decoded isKindOfClass:NSDictionary.class] ? decoded : @{};
}
static void SteamSaveCredentials(NSDictionary *credentials) {
    NSDictionary *query = @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: @"su.xash.svenclient.steam", (__bridge id)kSecAttrAccount: @"session"};
    if (!credentials) { SecItemDelete((__bridge CFDictionaryRef)query); return; }
    NSData *encoded = [NSJSONSerialization dataWithJSONObject:credentials options:0 error:nil];
    NSDictionary *values = @{(__bridge id)kSecValueData: encoded,
        (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)values);
    if (status == errSecItemNotFound) {
        NSMutableDictionary *entry = [query mutableCopy]; [entry addEntriesFromDictionary:values];
        status = SecItemAdd((__bridge CFDictionaryRef)entry, NULL);
    }
    if (status != errSecSuccess) throw std::runtime_error("Could not save the Steam session in Keychain");
}
static NSString *SteamString(const std::string &value) { return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding] ?: @""; }
static std::string SteamUTF8(NSString *value) { return value ? std::string(value.UTF8String ?: "") : std::string(); }
static std::string SteamEncryptPassword(NSString *password, const std::string &modulus, const std::string &exponent) {
    auto decode = [](std::string hex) {
        if (hex.empty() || hex.size() > 2048) throw std::runtime_error("Invalid Steam RSA key");
        if (hex.size() % 2) hex = "0" + hex;
        auto nibble = [](char character) -> uint8_t {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            throw std::runtime_error("Invalid Steam RSA key encoding");
        };
        Buffer result;
        for (size_t offset = 0; offset < hex.size(); offset += 2) result.push_back(uint8_t(nibble(hex[offset]) * 16 + nibble(hex[offset + 1])));
        return result;
    };
    Buffer publicModulus = decode(modulus), publicExponent = decode(exponent);
    NSData *der = derEncodeRSAPublicKey(publicModulus.data(), publicModulus.size(), publicExponent.data(), publicExponent.size());
    mbedtls_pk_context context; mbedtls_pk_init(&context);
    int result = mbedtls_pk_parse_public_key(&context, (const uint8_t *)der.bytes, der.length);
    psa_key_attributes_t attributes = psa_key_attributes_init();
    if (!result) result = mbedtls_pk_get_psa_attributes(&context, PSA_KEY_USAGE_ENCRYPT, &attributes);
    psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_CRYPT); psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    mbedtls_svc_key_id_t key = 0;
    if (!result) result = mbedtls_pk_import_into_psa(&context, &attributes, &key);
    mbedtls_pk_free(&context); psa_reset_key_attributes(&attributes);
    if (result) throw std::runtime_error("Steam password RSA key could not be loaded");
    NSData *plain = [password dataUsingEncoding:NSUTF8StringEncoding]; Buffer encrypted(publicModulus.size()); size_t length = 0;
    psa_status_t status = psa_asymmetric_encrypt(key, PSA_ALG_RSA_PKCS1V15_CRYPT, (const uint8_t *)plain.bytes, plain.length, NULL, 0, encrypted.data(), encrypted.size(), &length);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) throw std::runtime_error("Steam password encryption failed");
    return SteamUTF8([[NSData dataWithBytes:encrypted.data() length:length] base64EncodedStringWithOptions:0]);
}
static uint32_t SteamLocalIPv4(int socket) {
    sockaddr_in address = {}; socklen_t length = sizeof(address);
    if (getsockname(socket, (sockaddr *)&address, &length) || address.sin_family != AF_INET) return 0;
    return ntohl(address.sin_addr.s_addr);
}
static bool SteamSocketRead(int socket, void *output, size_t size) {
    auto *bytes = (uint8_t *)output;
    while (size) {
        ssize_t count = recv(socket, bytes, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        size -= size_t(count); bytes += count;
    }
    return true;
}
static bool SteamSocketWrite(int socket, const Buffer &data) {
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t count = send(socket, data.data() + offset, data.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += size_t(count);
    }
    return true;
}
}

@interface SteamService ()
@property (atomic, readwrite) BOOL loggedIn;
@property (atomic, readwrite) BOOL busy;
@property (atomic, readwrite) BOOL brokerRunning;
@property (atomic, copy, readwrite) NSString *accountName;
@property (atomic, copy, readwrite) NSString *status;
@property (atomic, copy, readwrite) NSString *guardKind;
- (void)closeP2P:(int)owner request:(uint64_t)request;
- (void)receiveP2P:(int)message body:(const Buffer &)body;
- (void)startP2P:(int)owner command:(const std::string &)command;
- (void)sendP2P:(int)owner payload:(const Buffer &)payload;
- (void)receive:(int)message body:(const Buffer &)body header:(const Buffer &)protoHeader client:(SteamCMClient *)client;
- (void)pollLogin:(uint64_t)generation deadline:(uint64_t)deadline;
- (void)advertise;
- (void)handleBroker:(int)socket;
- (void)becameActive;
@end

@implementation SteamService {
    SteamCMClient *_cm;
    dispatch_queue_t _operations;
    dispatch_source_t _heartbeat;
    std::atomic<uint64_t> _generation;
    std::atomic<bool> _connected;
    std::mutex _pendingMutex, _brokerMutex, _ticketMutex;
    std::map<uint64_t, std::shared_ptr<SteamPending>> _pending;
    std::deque<Buffer> _tokens;
    std::map<uint64_t, std::string> _friendAddresses;
    std::condition_variable _friendChanged, _tokensChanged;
    uint64_t _nextJob, _steamID, _authClientID, _authSteamID, _ticketSequence;
    int _sessionID, _brokerSocket, _guardType;
    Buffer _authRequest, _boundToken;
    std::string _boundAddress;
    uint64_t _boundServerID;
    unsigned _boundPort;
    bool _boundSecure;
    double _pollInterval;
    std::set<int> _brokerClients;
    std::map<uint32_t, Buffer> _tickets;
    std::mutex _p2pMutex;
    std::shared_ptr<P2P> _p2p;
    int _p2pOwner;
    uint64_t _p2pRequest, _p2pPeer;
}

+ (instancetype)shared {
    static SteamService *service; static dispatch_once_t once;
    dispatch_once(&once, ^{ service = [[SteamService alloc] init]; });
    return service;
}
- (instancetype)init {
    if ((self = [super init])) {
        _operations = dispatch_queue_create("su.xash.svenclient.steam", DISPATCH_QUEUE_SERIAL);
        _generation = 1; _connected = false; _nextJob = 1; _brokerSocket = -1;
        _status = @"Sign in to Steam"; _guardKind = @""; _accountName = SteamCredentials()[@"account"] ?: @"";
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"steam_auto_connect": @YES}];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(becameActive) name:UIApplicationDidBecomeActiveNotification object:nil];
    }
    return self;
}
- (BOOL)autoConnect { return [[NSUserDefaults standardUserDefaults] boolForKey:@"steam_auto_connect"]; }
- (BOOL)hasSavedSession { return [SteamCredentials()[@"refresh"] length] > 0; }
- (void)becameActive { if (self.autoConnect) [self restoreSession]; }
- (void)setAutoConnect:(BOOL)value { [[NSUserDefaults standardUserDefaults] setBool:value forKey:@"steam_auto_connect"]; }
- (void)notify {
    dispatch_async(dispatch_get_main_queue(), ^{ [[NSNotificationCenter defaultCenter] postNotificationName:SvenSteamStateChanged object:self]; });
}
- (void)fail:(const char *)message {
    self.busy = NO; self.guardKind = @""; self.status = [NSString stringWithUTF8String:message] ?: @"Steam connection failed"; [self notify];
}
- (void)closeConnection {
    _connected = false; self.loggedIn = NO;
    [self closeP2P:-1 request:0];
    if (_heartbeat) { dispatch_source_cancel(_heartbeat); _heartbeat = nil; }
    [_cm disconnect];
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _tokens.clear(); _friendAddresses.clear();
        for (auto &entry : _pending) { entry.second->done = true; entry.second->changed.notify_all(); }
        _tokensChanged.notify_all(); _friendChanged.notify_all();
    }
    { std::lock_guard<std::mutex> lock(_ticketMutex); _tickets.clear(); }
    @synchronized (self) { _boundServerID = 0; _boundPort = 0; _boundAddress.clear(); _boundToken.clear(); }
}
- (void)connectCM:(uint64_t)generation {
    if (generation != _generation) throw std::runtime_error("Steam login cancelled");
    [self closeConnection];
    _steamID = 0x0110000100000000ULL; _sessionID = 0;
    SteamCMClient *client = [[SteamCMClient alloc] initWithOnStatus:nil];
    client->_currentSteamId = _steamID; _cm = client;
    NSError *error = nil;
    if (![client connectWithError:&error] || ![client negotiateEncryptionWithError:&error])
        throw std::runtime_error(SteamUTF8(error.localizedDescription ?: @"Could not connect to Steam"));
    if (generation != _generation) { [client disconnect]; throw std::runtime_error("Steam login cancelled"); }
    timeval timeout = {}; setsockopt(client->_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    client->_readDeadline = UINT64_MAX;
    _connected = true;
    { std::lock_guard<std::mutex> lock(_pendingMutex); _tokens.clear(); _friendAddresses.clear(); }
    Buffer hello; number(hello, 1, 65581);
    [client sendProtobufMsg:9805 body:hello header:header(_steamID, 0)];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            while (generation == self->_generation && self->_connected && client == self->_cm) {
                int message; Buffer body, protoHeader; BOOL proto;
                if (![client readMessageWithEmsg:&message body:&body isProto:&proto header:&protoHeader error:nil]) break;
                try { [self receive:message body:body header:protoHeader client:client]; }
                catch (const std::exception &error) { NSLog(@"Steam: rejected malformed message %d: %s", message, error.what()); }
            }
            if (generation == self->_generation && client == self->_cm) {
                BOOL restore = self.loggedIn && self.autoConnect;
                [self closeConnection];
                self.status = @"Steam connection interrupted"; [self notify];
                if (restore) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), self->_operations, ^{
                    if (generation == self->_generation && !self->_connected) [self restoreSession];
                });
            }
        }
    });
}
- (void)send:(int)message body:(const Buffer &)body {
    if (!_connected) throw std::runtime_error("Steam connection interrupted. Please reconnect.");
    [_cm sendProtobufMsg:message body:body header:header(_steamID, _sessionID)];
}
- (SteamReply)request:(int)message body:(const Buffer &)body method:(const std::string &)method expected:(int)expected timeout:(int)seconds {
    auto pending = std::make_shared<SteamPending>(); pending->expected = expected;
    uint64_t job;
    { std::lock_guard<std::mutex> lock(_pendingMutex); job = _nextJob++; _pending[job] = pending; }
    if (!_connected) { std::lock_guard<std::mutex> lock(_pendingMutex); _pending.erase(job); throw std::runtime_error("Steam connection interrupted"); }
    try { [_cm sendProtobufMsg:message body:body header:header(_steamID, _sessionID, job, method)]; }
    catch (...) { std::lock_guard<std::mutex> lock(_pendingMutex); _pending.erase(job); throw; }
    std::unique_lock<std::mutex> lock(_pendingMutex);
    bool finished = pending->changed.wait_for(lock, std::chrono::seconds(seconds), [&]{ return pending->done; });
    _pending.erase(job);
    if (!finished) throw std::runtime_error("Steam request timed out. Check your connection and try again.");
    if (!pending->reply.message) throw std::runtime_error("Steam connection lost. Please try again.");
    return pending->reply;
}
- (Buffer)unified:(const std::string &)method body:(const Buffer &)body {
    auto reply = [self request:(self.loggedIn ? 151 : 9804) body:body method:method expected:147 timeout:60];
    int result = int(value(reply.header, 13, 1));
    if (result == 5) throw std::runtime_error("Check your Steam username and password.");
    if (result == 56 || result == 88 || result == 89) throw std::runtime_error("Check your Steam Guard code and try again.");
    if (result == 84) throw std::runtime_error("Too many Steam login attempts. Please try again later.");
    if (result != 1 && result != 23) throw std::runtime_error("Steam rejected this request (" + std::to_string(result) + ").");
    return reply.body;
}
- (void)receive:(int)message body:(const Buffer &)body header:(const Buffer &)protoHeader client:(SteamCMClient *)client {
    if (message == 1) {
        auto packets = [client parseMultiMessage:body];
        for (NSDictionary *packet in packets) {
            NSData *packetBody = packet[@"body"], *packetHeader = packet[@"header"];
            Buffer decodedBody((const uint8_t *)packetBody.bytes, (const uint8_t *)packetBody.bytes + packetBody.length);
            Buffer decodedHeader((const uint8_t *)packetHeader.bytes, (const uint8_t *)packetHeader.bytes + packetHeader.length);
            [self receive:[packet[@"emsg"] intValue] body:decodedBody header:decodedHeader client:client];
        }
        return;
    }
    if (message == 751 && value(body, 1) == 1) {
        _steamID = value(protoHeader, 1, _steamID); _sessionID = int(value(protoHeader, 2));
        client->_currentSteamId = _steamID; client->_currentSessionId = _sessionID;
    }
    if (message == 779) {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        for (const auto &field : fields(body)) if (field.id == 2 && field.wire == 2 && _tokens.size() < 64) _tokens.push_back(field.bytes);
        _tokensChanged.notify_all();
    }
    if (message == 766) {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        for (const auto &field : fields(body)) if (field.id == 2 && field.wire == 2 && value(field.bytes, 3) == 225840) {
            uint64_t friendID = value(field.bytes, 1); uint32_t ip = uint32_t(value(field.bytes, 4)); unsigned port = unsigned(value(field.bytes, 5));
            if (friendID && ip && port && port <= 65535) {
                _friendAddresses[friendID] = std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255) + ":" + std::to_string(port);
            }
        }
        _friendChanged.notify_all();
    }
    if (message == 5434 || message == 5435) [self receiveP2P:message body:body];
    if (message == 757) { [self closeConnection]; self.status = @"Steam signed out. Please sign in again."; [self notify]; return; }
    std::lock_guard<std::mutex> lock(_pendingMutex);
    uint64_t target = value(protoHeader, 11);
    auto match = _pending.find(target);
    if (match == _pending.end() && (!target || target == UINT64_MAX)) {
        for (auto entry = _pending.begin(); entry != _pending.end(); ++entry)
            if (entry->second->expected == message) { match = entry; break; }
    }
    if (match != _pending.end()) {
        match->second->reply = {message, body, protoHeader}; match->second->done = true; match->second->changed.notify_all();
    }
}
- (void)beginHeartbeat:(unsigned)seconds {
    if (_heartbeat) dispatch_source_cancel(_heartbeat);
    uint64_t generation = _generation;
    _heartbeat = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(_heartbeat, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), std::max(3u, seconds) * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(_heartbeat, ^{
        if (generation != self->_generation || !self.loggedIn) return;
        try { [self send:clientHeartbeat body:Buffer{}]; [self advertise]; }
        catch (...) {
            [self closeConnection];
            if (self.autoConnect) dispatch_async(self->_operations, ^{ if (generation == self->_generation) [self restoreSession]; });
        }
    });
    dispatch_resume(_heartbeat);
}
- (void)tokenLogin:(NSDictionary *)credentials generation:(uint64_t)generation {
    if (!_connected) [self connectCM:generation];
    Buffer body; number(body, 1, 65581); number(body, 5, 1771); text(body, 6, "english"); number(body, 7, uint64_t(int64_t(-600)));
    number(body, 8, 1); text(body, 50, SteamUTF8(credentials[@"account"])); text(body, 108, SteamUTF8(credentials[@"refresh"]));
    text(body, 96, "SvenClient iOS"); number(body, 102, 1);
    uint32_t privateIP = SteamLocalIPv4(_cm->_sock) ^ 0xbaadf00d;
    number(body, 2, privateIP); Buffer address; fixed(address, 1, privateIP, 4); data(body, 11, address);
    auto response = [self request:5514 body:body method:"" expected:751 timeout:90];
    if (generation != _generation) throw std::runtime_error("Steam login cancelled");
    if (value(response.body, 1) != 1) throw std::runtime_error("Steam rejected the saved session. Please sign in again.");
    self.loggedIn = YES; self.busy = NO; self.guardKind = @""; self.accountName = credentials[@"account"] ?: @"";
    self.status = @"Connected to Steam";
    Buffer online; number(online, 1, 1); [self send:716 body:online];
    [self beginHeartbeat:unsigned(value(response.body, 3, 9))]; [self startBroker]; [self notify];
}
- (void)login:(NSString *)username password:(NSString *)password {
    [self cancelLogin]; uint64_t generation = _generation;
    self.busy = YES; self.status = @"Connecting to Steam…"; [self notify];
    dispatch_async(_operations, ^{
        @autoreleasepool { try {
            [self connectCM:generation];
            Buffer request; text(request, 1, SteamUTF8(username));
            Buffer rsa = [self unified:"Authentication.GetPasswordRSAPublicKey#1" body:request];
            std::string encrypted = SteamEncryptPassword(password, string(rsa, 1), string(rsa, 2));
            Buffer begin, device;
            text(begin, 2, SteamUTF8(username)); text(begin, 3, encrypted); number(begin, 4, value(rsa, 3)); number(begin, 7, 1); text(begin, 8, "Client");
            text(device, 1, "SvenClient iOS"); number(device, 2, 1); number(device, 3, uint64_t(int64_t(-600))); data(begin, 9, device);
            NSString *guard = SteamCredentials()[@"guard"]; if (guard.length) text(begin, 10, SteamUTF8(guard));
            Buffer response = [self unified:"Authentication.BeginAuthSessionViaCredentials#1" body:begin];
            if (generation != self->_generation) return;
            self->_authClientID = value(response, 1); self->_authSteamID = value(response, 5); self->_authRequest = blob(response, 2);
            uint32_t intervalBits = uint32_t(value(response, 3)); float interval = 5; if (intervalBits) memcpy(&interval, &intervalBits, 4);
            self->_pollInterval = std::isfinite(interval) ? std::max(0.5, std::min(30.0, double(interval))) : 5;
            if (!self->_authClientID || self->_authRequest.empty()) throw std::runtime_error("Steam returned an invalid login session");
            std::set<unsigned> confirmations;
            for (const auto &field : fields(response)) if (field.id == 4 && field.wire == 2) confirmations.insert(unsigned(value(field.bytes, 1)));
            self.accountName = username;
            if (confirmations.count(1) || confirmations.count(4)) {
                self.guardKind = @"approval"; self.status = @"Approve this sign-in in the Steam mobile app"; [self notify];
                [self pollLogin:generation deadline:milliseconds() + 300000];
            } else {
                self->_guardType = confirmations.count(3) ? 3 : 2;
                self.guardKind = self->_guardType == 3 ? @"guard" : @"email";
                self.busy = NO; self.status = self->_guardType == 3 ? @"Enter your Steam Guard code" : @"Enter the code sent to your email"; [self notify];
            }
        } catch (const std::exception &error) { if (generation == self->_generation) { [self closeConnection]; [self fail:error.what()]; } } }
    });
}
- (void)submitCode:(NSString *)code {
    if (!_authClientID || self.busy) return;
    uint64_t generation = _generation; self.busy = YES; self.status = @"Verifying code…"; [self notify];
    dispatch_async(_operations, ^{
        try {
            Buffer request; number(request, 1, self->_authClientID); fixed(request, 2, self->_authSteamID);
            text(request, 3, SteamUTF8(code)); number(request, 4, self->_guardType);
            [self unified:"Authentication.UpdateAuthSessionWithSteamGuardCode#1" body:request];
            [self pollLogin:generation deadline:milliseconds() + 300000];
        } catch (const std::exception &error) {
            if (generation == self->_generation) {
                self.busy = NO; self.status = SteamString(error.what());
                if (!self->_connected) { self.guardKind = @""; self->_authClientID = 0; }
                [self notify];
            }
        }
    });
}
- (void)pollLogin:(uint64_t)generation deadline:(uint64_t)deadline {
    if (generation != _generation) return;
    if (milliseconds() >= deadline) { self.busy = NO; self.guardKind = @""; throw std::runtime_error("Steam approval timed out. Please try again."); }
    Buffer request; number(request, 1, _authClientID); data(request, 2, _authRequest);
    Buffer response = [self unified:"Authentication.PollAuthSessionStatus#1" body:request];
    if (generation != _generation) return;
    _authClientID = value(response, 1, _authClientID);
    std::string refresh = string(response, 3);
    if (!refresh.empty()) {
        NSString *account = SteamString(string(response, 6)); if (!account.length) account = self.accountName;
        NSDictionary *credentials = @{@"account":account, @"refresh":SteamString(refresh), @"guard":SteamString(string(response, 7)), @"steamID":@(_authSteamID)};
        SteamSaveCredentials(credentials); _authClientID = 0;
        [self tokenLogin:credentials generation:generation]; return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(_pollInterval * NSEC_PER_SEC)), _operations, ^{
        try { [self pollLogin:generation deadline:deadline]; }
        catch (const std::exception &error) { if (generation == self->_generation) [self fail:error.what()]; }
    });
}
- (void)restoreSession {
    if (self.busy || self.loggedIn) return;
    NSDictionary *credentials = SteamCredentials(); if (![credentials[@"refresh"] length]) return;
    [self cancelLogin]; uint64_t generation = _generation; self.busy = YES; self.status = @"Reconnecting to Steam…"; [self notify];
    dispatch_async(_operations, ^{
        try { [self tokenLogin:credentials generation:generation]; }
        catch (const std::exception &error) { if (generation == self->_generation) { [self closeConnection]; [self fail:error.what()]; } }
    });
}
- (void)cancelLogin {
    ++_generation; _authClientID = 0; [self closeConnection];
    self.busy = NO; self.guardKind = @""; self.status = @"Sign in to Steam"; [self notify];
}
- (void)logout {
    [self stopBroker]; [self cancelLogin]; SteamSaveCredentials(nil); self.accountName = @""; [self notify];
}
- (void)advertise {
    Buffer game, body;
    @synchronized (self) {
        if (_boundServerID) number(game, 1, _boundServerID);
        fixed(game, 2, 225840);
        in_addr address = {}; inet_pton(AF_INET, _boundAddress.c_str(), &address); uint32_t ip = ntohl(address.s_addr);
        if (ip) { number(game, 3, ip); Buffer nested; fixed(nested, 1, ip, 4); data(game, 23, nested); }
        if (_boundPort) { number(game, 4, _boundPort); number(game, 5, _boundSecure); }
        if (!_boundToken.empty()) data(game, 6, _boundToken);
        number(game, 9, 0); number(game, 12, uint32_t(_steamID));
    }
    data(body, 1, game); number(body, 2, 0); [self send:742 body:body]; [self send:5410 body:body];
}
- (Buffer)ticketForServer:(uint64_t)server address:(const std::string &)address port:(unsigned)port secure:(bool)secure {
    std::lock_guard<std::mutex> ticketLock(_ticketMutex);
    Buffer token;
    {
        std::unique_lock<std::mutex> lock(_pendingMutex);
        _tokensChanged.wait_for(lock, std::chrono::seconds(20), [&]{ return !_tokens.empty() || !_connected; });
        if (_tokens.empty()) throw std::runtime_error("Steam game-connect tokens are unavailable");
        token = _tokens.front(); _tokens.pop_front();
    }
    @synchronized (self) { _boundServerID = server; _boundAddress = address; _boundPort = port; _boundSecure = secure; _boundToken = token; }
    [self advertise];
    Buffer request; number(request, 1, 225840);
    auto ownership = [self request:857 body:request method:"" expected:858 timeout:30];
    Buffer appTicket = blob(ownership.body, 3);
    if (value(ownership.body, 1) != 1 || appTicket.empty()) throw std::runtime_error("Steam app ownership ticket unavailable");
    Buffer auth; integer(auth, token.size(), 4); append(auth, token); integer(auth, 24, 4); integer(auth, 1, 4); integer(auth, 2, 4);
    uint8_t addresses[8]; arc4random_buf(addresses, sizeof(addresses)); auth.insert(auth.end(), addresses, addresses + sizeof(addresses));
    integer(auth, milliseconds(), 4); integer(auth, ++_ticketSequence, 4);
    uint32_t checksum = uint32_t(crc32(0, auth.data(), uInt(auth.size())));
    _tickets[checksum] = auth;
    size_t tokensLeft;
    { std::lock_guard<std::mutex> lock(_pendingMutex); tokensLeft = _tokens.size(); }
    Buffer list; number(list, 1, tokensLeft); number(list, 5, 225840);
    for (const auto &entry : _tickets) { Buffer ticket; fixed(ticket, 4, 225840); number(ticket, 6, entry.first); data(ticket, 7, entry.second); data(list, 4, ticket); }
    [self request:5432 body:list method:"" expected:5575 timeout:30];
    Buffer combined; integer(combined, token.size(), 4); append(combined, token); integer(combined, appTicket.size(), 4); append(combined, appTicket);
    return combined;
}
- (void)brokerWrite:(int)socket payload:(const Buffer &)payload {
    Buffer frame = brokerFrame(payload);
    std::lock_guard<std::mutex> lock(_brokerMutex);
    if (!_brokerClients.count(socket) || !SteamSocketWrite(socket, frame)) throw std::runtime_error("Engine broker disconnected");
}
- (void)brokerText:(int)socket text:(const std::string &)text {
    [self brokerWrite:socket payload:Buffer(text.begin(), text.end())];
}
- (void)startBroker {
    if (self.brokerRunning || !self.loggedIn) return;
    int listener = socket(AF_INET, SOCK_STREAM, 0); if (listener < 0) return;
    int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_port = htons(27420); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (sockaddr *)&address, sizeof(address)) || listen(listener, 4)) { close(listener); self.status = @"Steam connected; could not start game broker"; [self notify]; return; }
    _brokerSocket = listener; self.brokerRunning = YES; [self notify];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        while (self.brokerRunning && self->_brokerSocket == listener) {
            int client = accept(listener, NULL, NULL); if (client < 0) break;
            int noSignal = 1; setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
            { std::lock_guard<std::mutex> lock(self->_brokerMutex); self->_brokerClients.insert(client); }
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ [self handleBroker:client]; });
        }
    });
}
- (void)stopBroker {
    self.brokerRunning = NO;
    if (_brokerSocket >= 0) { shutdown(_brokerSocket, SHUT_RDWR); close(_brokerSocket); _brokerSocket = -1; }
    [self closeP2P:-1 request:0];
    std::lock_guard<std::mutex> lock(_brokerMutex);
    for (int client : _brokerClients) shutdown(client, SHUT_RDWR);
    [self notify];
}
- (void)handleBroker:(int)socket {
    @autoreleasepool {
        try {
            uint8_t frameHeader[6];
            while (SteamSocketRead(socket, frameHeader, sizeof(frameHeader))) {
                Buffer wire(frameHeader, frameHeader + 6);
                if (memcmp(frameHeader, "SBRK", 4)) break;
                size_t length = size_t(readInteger(wire, 4, 2)); if (!length || length > 8192) break;
                Buffer payload(length); if (!SteamSocketRead(socket, payload.data(), length)) break;
                if (payload.size() >= 12 && !memcmp(payload.data(), "sb_p2p_send\n", 12)) { [self sendP2P:socket payload:payload]; continue; }
                std::string command(payload.begin(), payload.end()); std::istringstream parser(command); std::string name; parser >> name;
                if (name == "sb_connect") {
                    std::string serverAddress; uint64_t server = 0; unsigned secure = 0, app = 0; int64_t challenge = 0; uint64_t requestID = 0;
                    parser >> serverAddress >> server >> secure >> challenge >> app >> requestID;
                    if (app && app != 225840) continue;
                    std::string ip; unsigned port = 0;
                    if (serverAddress.compare(0, 6, "steam:")) { size_t colon = serverAddress.rfind(':'); ip = serverAddress.substr(0, colon); if (colon != std::string::npos) port = unsigned(std::stoul(serverAddress.substr(colon + 1))); }
                    try {
                        Buffer ticket = [self ticketForServer:server address:ip port:port secure:secure != 0];
                        Buffer reply = connectReply(uint32_t(challenge), uint32_t(requestID), _steamID, ticket);
                        [self brokerWrite:socket payload:reply];
                    } catch (const std::exception &error) { NSLog(@"Steam ticket: %s", error.what()); }
                } else if (name == "sb_masterlist") {
                    try {
                        Buffer query; number(query, 1, 225840); number(query, 3, 255); text(query, 4, "\\appid\\225840\\gamedir\\svencoop"); number(query, 5, 5000);
                        auto response = [self request:6403 body:query method:"" expected:6404 timeout:15];
                        auto addresses = servers(response.body);
                        for (const auto &entry : addresses) [self brokerText:socket text:"sb_masterlist_entry " + entry];
                        [self brokerText:socket text:"sb_masterlist_end " + std::to_string(addresses.size()) + " ok"];
                    } catch (...) { [self brokerText:socket text:"sb_masterlist_end 0 error"]; }
                } else if (name == "sb_friend") {
                    uint64_t friendID = 0; parser >> friendID;
                    Buffer request; number(request, 1, 20); fixed(request, 2, friendID); [self send:815 body:request];
                    std::unique_lock<std::mutex> lock(_pendingMutex);
                    _friendChanged.wait_for(lock, std::chrono::seconds(12), [&]{ return _friendAddresses.count(friendID) || !_connected; });
                    std::string address = _friendAddresses[friendID]; _friendAddresses.erase(friendID); lock.unlock();
                    [self brokerText:socket text:"sb_friend_result\n" + address];
                } else if (name == "sb_p2p_start") [self startP2P:socket command:command];
                else if (name == "sb_p2p_stop") { uint64_t request = 0; parser >> request; [self closeP2P:socket request:request]; }
                else if (name == "sb_disconnect") {
                    [self closeP2P:socket request:0];
                    @synchronized (self) { _boundServerID = 0; _boundPort = 0; _boundAddress.clear(); _boundToken.clear(); }
                    { std::lock_guard<std::mutex> ticketLock(_ticketMutex); _tickets.clear(); }
                    Buffer list; number(list, 5, 225840); [self send:5432 body:list];
                } else if (name == "sb_terminate") break;
            }
        } catch (const std::exception &error) { NSLog(@"Steam broker: %s", error.what()); }
        [self closeP2P:socket request:0];
        { std::lock_guard<std::mutex> lock(_brokerMutex); _brokerClients.erase(socket); }
        close(socket);
    }
}
- (void)closeP2P:(int)owner request:(uint64_t)request {
    std::shared_ptr<P2P> session;
    {
        std::lock_guard<std::mutex> lock(_p2pMutex);
        if (!_p2p || (owner >= 0 && owner != _p2pOwner) || (request && request != _p2pRequest)) return;
        session.swap(_p2p); _p2pRequest = 0; _p2pOwner = -1;
    }
    session->close();
}
- (void)startP2P:(int)owner command:(const std::string &)command {
    std::istringstream parser(command); std::string name; uint64_t request = 0, peer = 0; unsigned app = 0;
    parser >> name >> request >> peer >> app;
    if (!request || request > UINT32_MAX || !peer || peer == _steamID || app != 225840 || !self.loggedIn) {
        [self brokerText:owner text:"sb_p2p_error " + std::to_string(request) + " invalid_request_or_steam_session"]; return;
    }
    [self closeP2P:-1 request:0];
    try {
        [self brokerText:owner text:"sb_p2p_state " + std::to_string(request) + " starting"];
        auto session = std::make_shared<P2P>(_steamID, peer,
            [self](const Buffer &signal){ [self send:5434 body:signal]; },
            [self, owner, request, peer](const Buffer &packet){
                std::string prefix = "sb_p2p_data\n"; Buffer body(prefix.begin(), prefix.end()); integer(body, request, 4); integer(body, peer, 8); append(body, packet);
                [self brokerWrite:owner payload:body];
            },
            [self, owner, request](const std::string &state){ [self brokerText:owner text:"sb_p2p_state " + std::to_string(request) + " " + state]; },
            [self, owner, request](const std::string &reason){ try { [self brokerText:owner text:"sb_p2p_error " + std::to_string(request) + " " + reason]; } catch (...) {} });
        { std::lock_guard<std::mutex> lock(_p2pMutex); _p2p = session; _p2pOwner = owner; _p2pRequest = request; _p2pPeer = peer; }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ session->run(); });
    } catch (const std::exception &error) { [self brokerText:owner text:"sb_p2p_error " + std::to_string(request) + " " + error.what()]; }
}
- (void)receiveP2P:(int)message body:(const Buffer &)body {
    std::shared_ptr<P2P> session;
    int owner; uint64_t request;
    { std::lock_guard<std::mutex> lock(_p2pMutex); session = _p2p; owner = _p2pOwner; request = _p2pRequest; }
    if (session) {
        try { session->receiveSignal(message, body); }
        catch (const std::exception &error) {
            try { [self brokerText:owner text:"sb_p2p_error " + std::to_string(request) + " " + error.what()]; } catch (...) {}
            [self closeP2P:owner request:request];
        }
    }
}
- (void)sendP2P:(int)owner payload:(const Buffer &)payload {
    if (payload.size() <= 24) return;
    uint64_t request = readInteger(payload, 12, 4), peer = readInteger(payload, 16, 8);
    std::shared_ptr<P2P> session;
    { std::lock_guard<std::mutex> lock(_p2pMutex); if (owner == _p2pOwner && request == _p2pRequest && peer == _p2pPeer) session = _p2p; }
    if (session) session->send(Buffer(payload.begin() + 24, payload.end()));
}
@end
