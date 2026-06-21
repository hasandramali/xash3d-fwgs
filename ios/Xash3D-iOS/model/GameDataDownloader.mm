#import "GameDataDownloader.h"
#import <os/log.h>
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <zlib.h>
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include <mbedtls/private/aes.h>
#include <mbedtls/private/rsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <psa/crypto.h>

extern "C" {
#include "../../../3rdparty/lzma/lzma.h"
}

#pragma mark - Constants

static const int PROTO_MASK = 0x80000000;
static const int TCP_MAGIC = 0x31305456;
static const uint64_t ANON_STEAM_ID = (10ULL << 52) | (1ULL << 56);
static const int CM_PORT = 27017;

static NSArray *CM_SERVERS() {
    return @[@"162.254.197.40", @"162.254.199.170",
             @"155.133.248.34", @"155.133.248.35", @"155.133.246.43"];
}

static NSDictionary *HARDCODED_MANIFEST_IDS() {
    return @{@71: @9183617604528345869ULL, @11: @4720911300072406946ULL,
             @21: @7841127166138118042ULL, @51: @789184054796507140ULL,
             @81: @3601230779843470737ULL, @1:  @5928322771446233610ULL,
             @4:  @8690279432129063737ULL, @1006:@6912453647411644579ULL};
}

static NSDictionary *GAMES() {
    return @{@"valve":   @{@"appId":@90, @"depotIds":@[@71,@4,@1],  @"displayName":@"Half-Life"},
             @"cstrike": @{@"appId":@90, @"depotIds":@[@11],        @"displayName":@"Counter-Strike"},
             @"tfc":     @{@"appId":@90, @"depotIds":@[@21],        @"displayName":@"Team Fortress Classic"},
             @"gearbox": @{@"appId":@90, @"depotIds":@[@51],        @"displayName":@"Opposing Force"},
             @"czero":   @{@"appId":@90, @"depotIds":@[@81],        @"displayName":@"Condition Zero"}};
}

static NSData *steamPublicKey() {
    static const uint8_t k[] = {
        0x30,0x81,0x9D,0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01,
        0x05,0x00,0x03,0x81,0x8B,0x00,0x30,0x81,0x87,0x02,0x81,0x81,0x00,0xDF,0xEC,0x1A,
        0xD6,0x2C,0x10,0x66,0x2C,0x17,0x35,0x3A,0x14,0xB0,0x7C,0x59,0x11,0x7F,0x9D,0xD3,
        0xD8,0x2B,0x7A,0xE3,0xE0,0x15,0xCD,0x19,0x1E,0x46,0xE8,0x7B,0x87,0x74,0xA2,0x18,
        0x46,0x31,0xA9,0x03,0x14,0x79,0x82,0x8E,0xE9,0x45,0xA2,0x49,0x12,0xA9,0x23,0x68,
        0x73,0x89,0xCF,0x69,0xA1,0xB1,0x61,0x46,0xBD,0xC1,0xBE,0xBF,0xD6,0x01,0x1B,0xD8,
        0x81,0xD4,0xDC,0x90,0xFB,0xFE,0x4F,0x52,0x73,0x66,0xCB,0x95,0x70,0xD7,0xC5,0x8E,
        0xBA,0x1C,0x7A,0x33,0x75,0xA1,0x62,0x34,0x46,0xBB,0x60,0xB7,0x80,0x68,0xFA,0x13,
        0xA7,0x7A,0x8A,0x37,0x4B,0x9E,0xC6,0xF4,0x5D,0x5F,0x3A,0x99,0xF9,0x9E,0xC4,0x3A,
        0xE9,0x63,0xA2,0xBB,0x88,0x19,0x28,0xE0,0xE7,0x14,0xC0,0x42,0x89,0x02,0x01,0x11};
    return [NSData dataWithBytes:k length:sizeof(k)];
}

#pragma mark - Logging

static NSString *_logPath = nil;
static os_log_t _gddLog = nil;

static void ensureOsLog() {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        _gddLog = os_log_create("com.xash3d.gamedatadownloader", "SteamCDN");
    });
}

static void initLogPath(NSString *docsDir) {
    ensureOsLog();
    os_log(_gddLog, "GameDataDownloader initialized, docsDir=%{public}@", docsDir);
    _logPath = [[docsDir stringByAppendingPathComponent:@"xash_ios.log"] copy];
}

static void logToFile(NSString *format, ...) {
    if (!_logPath) return;
    va_list args;
    va_start(args, format);
    NSString *msg = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    // Also log to os_log for Console.app visibility
    ensureOsLog();
    os_log(_gddLog, "%{public}@", msg);
    // Write to xash_ios.txt
    NSFileHandle *fh = [NSFileHandle fileHandleForWritingAtPath:_logPath];
    if (!fh) {
        [[NSFileManager defaultManager] createFileAtPath:_logPath contents:nil attributes:nil];
        fh = [NSFileHandle fileHandleForWritingAtPath:_logPath];
    }
    if (fh) {
        [fh seekToEndOfFile];
        NSString *line = [NSString stringWithFormat:@"[%@] %@\n", [NSDateFormatter localizedStringFromDate:[NSDate date] dateStyle:NSDateFormatterNoStyle timeStyle:NSDateFormatterMediumStyle], msg];
        [fh writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
        [fh closeFile];
    }
}

#pragma mark - Proto helpers

typedef std::vector<uint8_t> bytes;

struct SteamProto {
    static uint64_t readVarint(const uint8_t *d, size_t &p, size_t l) {
        uint64_t v = 0; int s = 0;
        while (p < l) { uint8_t b = d[p++]; v |= (uint64_t)(b & 0x7F) << s; s += 7; if (!(b & 0x80)) return v; }
        return v;
    }
};

static void skipProtoField(const uint8_t *d, size_t &p, size_t l) {
    if (p >= l) return;
    uint64_t tag = SteamProto::readVarint(d, p, l);
    int wt = (int)(tag & 7);
    switch (wt) {
        case 0: SteamProto::readVarint(d, p, l); break;
        case 1: p += 8; break;
        case 2: { uint64_t sl = SteamProto::readVarint(d, p, l); p += (size_t)sl; } break;
        case 5: p += 4; break;
        default: break;
    }
}

static bytes packVarint(int64_t val) {
    bytes buf; uint64_t v = (uint64_t)val;
    while (v >= 0x80) { buf.push_back((uint8_t)(v & 0x7F) | 0x80); v >>= 7; }
    buf.push_back((uint8_t)(v & 0x7F));
    return buf;
}
static bytes packVarint32(int32_t val) {
    bytes buf; uint32_t v = (uint32_t)val;
    while (v >= 0x80u) { buf.push_back((uint8_t)(v & 0x7F) | 0x80); v >>= 7; }
    buf.push_back((uint8_t)(v & 0x7F));
    return buf;
}
static bytes packVarint64(uint64_t val) {
    bytes buf;
    while (val >= 0x80) { buf.push_back((uint8_t)(val & 0x7F) | 0x80); val >>= 7; }
    buf.push_back((uint8_t)(val & 0x7F));
    return buf;
}
static bytes packFixed32(int32_t v) {
    bytes buf(4);
    buf[0] = (uint8_t)(v & 0xFF); buf[1] = (uint8_t)((v>>8)&0xFF);
    buf[2] = (uint8_t)((v>>16)&0xFF); buf[3] = (uint8_t)((v>>24)&0xFF);
    return buf;
}
static bytes packFixed64(uint64_t v) {
    bytes buf(8);
    for (int i = 0; i < 8; i++) { buf[i] = (uint8_t)(v & 0xFF); v >>= 8; }
    return buf;
}
static int32_t readInt32LE(const uint8_t *d, size_t o) {
    return (int32_t)(d[o] | (d[o+1]<<8) | (d[o+2]<<16) | (d[o+3]<<24));
}
static int64_t readInt64LE(const uint8_t *d, size_t o) {
    uint64_t v = (uint64_t)d[o] | ((uint64_t)d[o+1]<<8) | ((uint64_t)d[o+2]<<16) | ((uint64_t)d[o+3]<<24)
               | ((uint64_t)d[o+4]<<32) | ((uint64_t)d[o+5]<<40) | ((uint64_t)d[o+6]<<48) | ((uint64_t)d[o+7]<<56);
    return (int64_t)v;
}
static int32_t adler32_steam(const uint8_t *d, size_t l) {
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < l; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (int32_t)(a | (b << 16));
}

static int mbedtls_rand(void *ctx, uint8_t *out, size_t len) {
    arc4random_buf(out, len); return 0;
}

static NSData *hmacSha1(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t dataLen) {
    uint8_t hash[20];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, keyLen, data, dataLen, hash);
    return [NSData dataWithBytes:hash length:20];
}

#pragma mark - AES helpers (Steam wire format)

static bytes encryptAES(const uint8_t *sk, const bytes &pt) {
    uint8_t r3[3]; arc4random_buf(r3, 3);
    NSData *hk = [NSData dataWithBytes:sk length:16];
    NSMutableData *hi = [NSMutableData dataWithBytes:r3 length:3];
    [hi appendBytes:pt.data() length:pt.size()];
    NSData *h = hmacSha1((const uint8_t *)hk.bytes, 16, (const uint8_t *)hi.bytes, hi.length);
    const uint8_t *hb = (const uint8_t *)h.bytes;
    uint8_t iv[16]; memcpy(iv, hb, 13); memcpy(iv+13, r3, 3);
    uint8_t ecbIv[16];
    mbedtls_aes_context ae; mbedtls_aes_init(&ae); mbedtls_aes_setkey_enc(&ae, sk, 256);
    mbedtls_aes_crypt_ecb(&ae, MBEDTLS_AES_ENCRYPT, iv, ecbIv); mbedtls_aes_free(&ae);
    size_t bc = (pt.size() + 15) / 16, pl = bc * 16;
    bytes pad(pl, 0); memcpy(pad.data(), pt.data(), pt.size());
    uint8_t pv = (uint8_t)(pl - pt.size()); memset(pad.data() + pt.size(), pv, pl - pt.size());
    bytes ct(pl);
    mbedtls_aes_context ac; mbedtls_aes_init(&ac); mbedtls_aes_setkey_enc(&ac, sk, 256);
    uint8_t ci[16]; memcpy(ci, iv, 16);
    mbedtls_aes_crypt_cbc(&ac, MBEDTLS_AES_ENCRYPT, pl, ci, pad.data(), ct.data());
    mbedtls_aes_free(&ac);
    bytes r(16 + ct.size()); memcpy(r.data(), ecbIv, 16); memcpy(r.data()+16, ct.data(), ct.size());
    return r;
}

static bytes decryptAES(const uint8_t *sk, const uint8_t *d, size_t l) {
    if (l < 16) throw std::runtime_error("too short");
    uint8_t iv[16];
    mbedtls_aes_context ae; mbedtls_aes_init(&ae); mbedtls_aes_setkey_dec(&ae, sk, 256);
    mbedtls_aes_crypt_ecb(&ae, MBEDTLS_AES_DECRYPT, d, iv); mbedtls_aes_free(&ae);
    size_t bl = l - 16; bytes pt(bl);
    mbedtls_aes_context ac; mbedtls_aes_init(&ac); mbedtls_aes_setkey_dec(&ac, sk, 256);
    uint8_t ci[16]; memcpy(ci, iv, 16);
    mbedtls_aes_crypt_cbc(&ac, MBEDTLS_AES_DECRYPT, bl, ci, d+16, pt.data());
    mbedtls_aes_free(&ac);
    // PKCS7 unpad FIRST (matching Android Cipher.doFinal behavior)
    size_t ps = bl;
    if (bl > 0) { uint8_t pv = pt[bl-1];
        if (pv > 0 && pv <= 16 && pv <= bl) { bool ok = true;
            for (size_t i = bl - pv; i < bl; i++) if (pt[i] != pv) { ok=false; break; }
            if (ok) ps = bl - pv; } }
    // HMAC verification on UNPADDED data (matching Android)
    uint8_t r3[3]; memcpy(r3, iv+13, 3);
    NSData *hk = [NSData dataWithBytes:sk length:16];
    NSMutableData *hi = [NSMutableData dataWithBytes:r3 length:3];
    [hi appendBytes:pt.data() length:ps];
    NSData *h = hmacSha1((const uint8_t *)hk.bytes, 16, (const uint8_t *)hi.bytes, hi.length);
    if (memcmp((const uint8_t *)h.bytes, iv, 13) != 0) throw std::runtime_error("HMAC fail");
    pt.resize(ps);
    return pt;
}

#pragma mark - RSA OAEP

// Build a DER-encoded PKCS#1 RSAPublicKey from raw modulus + exponent
static NSData *derEncodeRSAPublicKey(const uint8_t *mod, size_t modLen, const uint8_t *exp, size_t expLen) {
    // Each ASN.1 INTEGER: 02 len [bytes]  (add leading 0x00 if high bit set)
    auto prependInt = [](bytes &buf, const uint8_t *val, size_t len) {
        buf.insert(buf.begin(), val, val + len);
        if (buf.size() > 0 && (buf[0] & 0x80)) buf.insert(buf.begin(), 0x00);
        size_t ilen = buf.size();
        if (ilen < 128) { buf.insert(buf.begin(), (uint8_t)ilen); }
        else { bytes lb; size_t t = ilen; while (t) { lb.insert(lb.begin(), (uint8_t)(t & 0xFF)); t >>= 8; }
            lb.insert(lb.begin(), (uint8_t)(0x80 | lb.size())); buf.insert(buf.begin(), lb.begin(), lb.end()); }
        buf.insert(buf.begin(), 0x02);
    };
    bytes expDer, modDer;
    prependInt(expDer, exp, expLen);
    prependInt(modDer, mod, modLen);
    bytes seq;
    seq.insert(seq.end(), modDer.begin(), modDer.end());
    seq.insert(seq.end(), expDer.begin(), expDer.end());
    size_t slen = seq.size();
    if (slen < 128) { seq.insert(seq.begin(), (uint8_t)slen); }
    else { bytes lb; size_t t = slen; while (t) { lb.insert(lb.begin(), (uint8_t)(t & 0xFF)); t >>= 8; }
        lb.insert(lb.begin(), (uint8_t)(0x80 | lb.size())); seq.insert(seq.begin(), lb.begin(), lb.end()); }
    seq.insert(seq.begin(), 0x30);
    return [NSData dataWithBytes:seq.data() length:seq.size()];
}

static NSData *rsaEncryptOAEP(const uint8_t *pk, size_t pl, const uint8_t *d, size_t dl) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ psa_crypto_init(); });

    mbedtls_pk_context ctx; mbedtls_pk_init(&ctx);
    int r = mbedtls_pk_parse_public_key(&ctx, pk, pl);

    // If DER parsing fails, try custom format: [1-byte expLen][exp][mod]
    if (r != 0 && pl >= 3) {
        size_t expLen = pk[0];
        if (1 + expLen < pl) {
            NSData *derKey = derEncodeRSAPublicKey(pk + 1 + expLen, pl - 1 - expLen, pk + 1, expLen);
            if (derKey) {
                mbedtls_pk_free(&ctx);
                r = mbedtls_pk_parse_public_key(&ctx, (const uint8_t *)derKey.bytes, derKey.length);
            }
        }
    }
    if (r != 0) { mbedtls_pk_free(&ctx); return nil; }

    psa_key_attributes_t attributes = psa_key_attributes_init();
    r = mbedtls_pk_get_psa_attributes(&ctx, PSA_KEY_USAGE_ENCRYPT, &attributes);
    if (r != 0) { mbedtls_pk_free(&ctx); return nil; }
    // mbedtls_pk_get_psa_attributes sets algorithm to PKCS1V15_SIGN;
    // override for OAEP encryption
    psa_set_key_algorithm(&attributes, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1));
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);

    mbedtls_svc_key_id_t key_id = 0;
    r = mbedtls_pk_import_into_psa(&ctx, &attributes, &key_id);
    mbedtls_pk_free(&ctx);
    if (r != 0) return nil;

    size_t kl = (psa_get_key_bits(&attributes) + 7) / 8;
    NSMutableData *res = [NSMutableData dataWithLength:kl];
    size_t olen = 0;
    psa_status_t ps = psa_asymmetric_encrypt(key_id, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1),
                                              d, dl, NULL, 0,
                                              (uint8_t *)res.mutableBytes, kl, &olen);
    psa_destroy_key(key_id);
    if (ps != PSA_SUCCESS) return nil;
    res.length = olen;
    return res;
}

#pragma mark - Chunk decrypt (depot key)

static NSData *decryptChunk(const uint8_t *dk, size_t kl, const uint8_t *e, size_t el) {
    if (el < 16) return nil;
    uint8_t iv[16];
    mbedtls_aes_context ae; mbedtls_aes_init(&ae); mbedtls_aes_setkey_dec(&ae, dk, (unsigned)kl*8);
    mbedtls_aes_crypt_ecb(&ae, MBEDTLS_AES_DECRYPT, e, iv); mbedtls_aes_free(&ae);
    size_t bl = el - 16;
    NSMutableData *pt = [NSMutableData dataWithLength:bl];
    mbedtls_aes_context ac; mbedtls_aes_init(&ac); mbedtls_aes_setkey_dec(&ac, dk, (unsigned)kl*8);
    uint8_t ci[16]; memcpy(ci, iv, 16);
    mbedtls_aes_crypt_cbc(&ac, MBEDTLS_AES_DECRYPT, bl, ci, e+16, (uint8_t *)pt.mutableBytes);
    mbedtls_aes_free(&ac);
    if (bl > 0) { uint8_t p = ((const uint8_t *)pt.bytes)[bl-1];
        if (p > 0 && p <= 16) { size_t ps = bl - p; bool ok = true;
            for (size_t i = ps; i < bl; i++) if (((const uint8_t *)pt.bytes)[i] != p) { ok=false; break; }
            if (ok) pt.length = ps; } }
    return pt;
}

#pragma mark - Filename decrypt

static NSString *decryptFilename(const uint8_t *e, size_t el, NSData *dk) {
    NSString *raw = [[NSString alloc] initWithBytes:e length:el encoding:NSUTF8StringEncoding];
    if (!raw) return nil;
    NSString *norm = [[[[raw stringByReplacingOccurrencesOfString:@"+" withString:@"-"]
                        stringByReplacingOccurrencesOfString:@"/" withString:@"_"]
                       stringByReplacingOccurrencesOfString:@"\n" withString:@""]
                      stringByReplacingOccurrencesOfString:@" " withString:@""];
    NSData *dec = [[NSData alloc] initWithBase64EncodedString:norm options:NSDataBase64DecodingIgnoreUnknownCharacters];
    if (dec.length < 16) return raw;
    uint8_t iv[16];
    mbedtls_aes_context ae; mbedtls_aes_init(&ae);
    mbedtls_aes_setkey_dec(&ae, (const uint8_t *)dk.bytes, (unsigned)dk.length*8);
    mbedtls_aes_crypt_ecb(&ae, MBEDTLS_AES_DECRYPT, (const uint8_t *)dec.bytes, iv);
    mbedtls_aes_free(&ae);
    size_t bl = dec.length - 16;
    NSMutableData *pt = [NSMutableData dataWithLength:bl];
    mbedtls_aes_context ac; mbedtls_aes_init(&ac);
    mbedtls_aes_setkey_dec(&ac, (const uint8_t *)dk.bytes, (unsigned)dk.length*8);
    uint8_t ci[16]; memcpy(ci, iv, 16);
    mbedtls_aes_crypt_cbc(&ac, MBEDTLS_AES_DECRYPT, bl, ci, (const uint8_t *)dec.bytes+16, (uint8_t *)pt.mutableBytes);
    mbedtls_aes_free(&ac);
    // Strip PKCS7 padding (Android's Cipher PKCS5Padding does this automatically)
    if (bl > 0) {
        uint8_t p = ((const uint8_t *)pt.bytes)[bl-1];
        if (p > 0 && p <= 16) {
            size_t ps = bl - p; bool ok = true;
            for (size_t i = ps; i < bl; i++) if (((const uint8_t *)pt.bytes)[i] != p) { ok=false; break; }
            if (ok) pt.length = ps;
        }
    }
    NSString *r = [[NSString alloc] initWithData:pt encoding:NSUTF8StringEncoding];
    return r ? [r stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\0"]] : raw;
}

#pragma mark - Unzip from memory using libzip temp file

static NSData *unzipFirstEntry(NSData *zipData) {
    if (zipData.length < 22) return nil;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipData.bytes, zipData.length, 0)) return nil;
    NSData *result = nil;
    mz_uint cnt = mz_zip_reader_get_num_files(&zip);
    if (cnt > 0) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&zip, 0, &stat) && stat.m_uncomp_size > 0) {
            size_t outSize = 0;
            void *data = mz_zip_reader_extract_to_heap(&zip, 0, &outSize, 0);
            if (data) {
                result = [NSData dataWithBytesNoCopy:data length:outSize freeWhenDone:YES];
            }
        }
    }
    mz_zip_reader_end(&zip);
    return result;
}

#pragma mark - GZIP decompression

static NSData *gzipDecompress(NSData *data) {
    if (!data || data.length == 0) return nil;
    z_stream strm; memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)data.bytes;
    strm.avail_in = (uInt)data.length;
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return nil;
    NSMutableData *res = [NSMutableData dataWithLength:data.length * 4];
    int ret;
    do {
        if (res.length < strm.total_out + 65536) res.length = strm.total_out + 65536;
        strm.next_out = (Bytef *)res.mutableBytes + strm.total_out;
        strm.avail_out = (uInt)(res.length - strm.total_out);
        ret = inflate(&strm, Z_FINISH);
    } while (ret == Z_OK);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END) return nil;
    res.length = strm.total_out;
    return res;
}

#pragma mark - DNS resolution filter

static NSArray *filterResolvableHosts(NSArray *hosts) {
    NSMutableArray *res = [NSMutableArray array];
    for (NSString *host in hosts) {
        CFHostRef h = CFHostCreateWithName(NULL, (__bridge CFStringRef)host);
        if (!h) continue;
        Boolean ok = CFHostStartInfoResolution(h, kCFHostAddresses, NULL);
        CFRelease(h);
        if (ok) [res addObject:host];
        else NSLog(@"[cdn] Skipping unresolvable host: %@", host);
    }
    return res;
}

#pragma mark - Steam CM Client

@interface SteamCMClient : NSObject {
    int _sock;
    uint8_t _sessionKey[32];
    BOOL _encrypted;
    int _currentSessionId;
    uint64_t _currentSteamId;
    void (^_onStatus)(NSString *);
    NSThread *_heartbeatThread;
    int _heartbeatSeconds;
    BOOL _heartbeatRunning;
    int64_t _nextJobId;
}
- (instancetype)initWithOnStatus:(void(^)(NSString *))os;
- (BOOL)connectWithError:(NSError **)error;
- (BOOL)anonymousLoginWithError:(NSError **)error;
- (BOOL)requestLicense:(int)appId error:(NSError **)error;
- (NSArray *)requestCDNServerListWithError:(NSError **)error;
- (NSData *)getDepotDecryptionKey:(int)appId depotId:(int)depotId error:(NSError **)error;
- (uint64_t)requestManifestRequestCode:(int)depotId appId:(int)appId manifestId:(int64_t)manifestId error:(NSError **)error;
- (void)parseLogonResponseHeader:(NSData *)header;
- (void)disconnect;
@end

@implementation SteamCMClient

- (instancetype)initWithOnStatus:(void(^)(NSString *))os {
    if (self = [super init]) { _sock = -1; _encrypted = NO; _currentSessionId = 0; _currentSteamId = ANON_STEAM_ID; _onStatus = os; _heartbeatSeconds = 9; _heartbeatRunning = NO; _nextJobId = 1; }
    return self;
}
- (void)dealloc { [self stopHeartbeat]; [self disconnect]; }
- (void)status:(NSString *)s { if (_onStatus) _onStatus(s); }

- (BOOL)writeExact:(const uint8_t *)d length:(size_t)l {
    size_t o = 0; while (o < l) { ssize_t n = write(_sock, d+o, l-o); if (n > 0) o += n; else if (n == 0) return NO; else if (errno == EAGAIN || errno == EINTR) continue; else { logToFile(@"writeExact failed at %zu/%zu errno=%d", o, l, errno); return NO; } }
    return YES;
}
- (BOOL)readExact:(uint8_t *)d length:(size_t)l {
    size_t o = 0; int againRetries = 0;
    while (o < l) { ssize_t n = read(_sock, d+o, l-o); if (n > 0) { o += n; againRetries = 0; } else if (n == 0) return NO; else if (errno == EINTR) continue; else if (errno == EAGAIN) { if (++againRetries > 3) { logToFile(@"readExact: too many EAGAIN retries at %zu/%zu", o, l); return NO; } continue; } else { logToFile(@"readExact failed at %zu/%zu errno=%d", o, l, errno); return NO; } }
    return YES;
}

- (BOOL)connectWithError:(NSError **)error {
    for (NSString *host in CM_SERVERS()) {
        struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(CM_PORT);
        if (inet_pton(AF_INET, host.UTF8String, &a.sin_addr) != 1) { logToFile(@"Cannot parse %@", host); continue; }
        int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) { logToFile(@"socket() failed for %@", host); continue; }
        struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        logToFile(@"Trying CM server %@...", host);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) { _sock = s; logToFile(@"Connected to %@", host); [self status:[NSString stringWithFormat:@"Connected to %@", host]]; return YES; }
        logToFile(@"Connect to %@ failed (errno=%d)", host, errno);
        close(s);
    }
    if (error) *error = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"No Steam CM server reachable"}];
    return NO;
}

- (void)disconnect { [self stopHeartbeat]; if (_sock >= 0) { close(_sock); _sock = -1; logToFile(@"Disconnected"); } }

- (void)sendRaw:(const bytes &)d {
    uint32_t len = (uint32_t)d.size(), magic = TCP_MAGIC;
    uint8_t h[8]; memcpy(h, &len, 4); memcpy(h+4, &magic, 4);
    logToFile(@"sendRaw: len=%u magic=0x%08x", len, magic);
    [self writeExact:h length:8]; [self writeExact:d.data() length:d.size()];
}

- (void)sendProtobufMsg:(int)emsg body:(const bytes &)body header:(const bytes &)header {
    logToFile(@"sendProtobufMsg: emsg=%d header=%zu body=%zu encrypted=%d", emsg, header.size(), body.size(), _encrypted);
    bytes p; auto ep = packFixed32(emsg|PROTO_MASK); p.insert(p.end(), ep.begin(), ep.end());
    auto hl = packFixed32((int32_t)header.size()); p.insert(p.end(), hl.begin(), hl.end());
    p.insert(p.end(), header.begin(), header.end()); p.insert(p.end(), body.begin(), body.end());
    logToFile(@"  plain pdu size=%zu", p.size());
    bytes fp = _encrypted ? encryptAES(_sessionKey, p) : p;
    logToFile(@"  wire size=%zu", fp.size());
    [self sendRaw:fp];
}

#pragma mark - Heartbeat

- (void)startHeartbeat:(int)intervalSeconds {
    [self stopHeartbeat];
    _heartbeatSeconds = intervalSeconds;
    _heartbeatRunning = YES;
    __weak typeof(self) ws = self;
    _heartbeatThread = [[NSThread alloc] initWithBlock:^{
        while (![NSThread currentThread].isCancelled) {
            [NSThread sleepForTimeInterval:_heartbeatSeconds];
            if ([NSThread currentThread].isCancelled) break;
            __strong typeof(ws) ss = ws;
            if (!ss) break;
            bytes body; auto f = packVarint((1<<3)|0); auto v = packVarint(1);
            body.insert(body.end(),f.begin(),f.end()); body.insert(body.end(),v.begin(),v.end());
            bytes hdr; auto h1 = packVarint((1<<3)|1); auto si = packFixed64(_currentSteamId);
            hdr.insert(hdr.end(),h1.begin(),h1.end()); hdr.insert(hdr.end(),si.begin(),si.end());
            auto h2 = packVarint((2<<3)|0); auto se = packVarint32(_currentSessionId);
            hdr.insert(hdr.end(),h2.begin(),h2.end()); hdr.insert(hdr.end(),se.begin(),se.end());
            logToFile(@"Sending heartbeat (sessionId=%d steamId=%llu)", ss->_currentSessionId, (unsigned long long)ss->_currentSteamId);
            [ss sendProtobufMsg:1009 body:body header:hdr];
        }
    }];
    _heartbeatThread.name = @"SteamCM-Heartbeat";
    [_heartbeatThread start];
}

- (void)stopHeartbeat {
    _heartbeatRunning = NO;
    if (_heartbeatThread) {
        [_heartbeatThread cancel];
        _heartbeatThread = nil;
    }
}

#pragma mark - Multi (emsg=1) message parsing

- (NSArray<NSDictionary *> *)parseMultiMessage:(const bytes &)body {
    size_t p = 0; uint64_t sizeUnzipped = 0; bytes msgBody;
    while (p < body.size()) {
        uint64_t tag = SteamProto::readVarint(body.data(), p, body.size());
        int fn = (int)(tag>>3), wt = (int)(tag&7);
        if (fn == 1 && wt == 0) sizeUnzipped = SteamProto::readVarint(body.data(), p, body.size());
        else if (fn == 2 && wt == 2) { uint64_t l = SteamProto::readVarint(body.data(), p, body.size());
            msgBody.assign(body.data()+p, body.data()+p+l); p += l; }
        else { if (wt==0) SteamProto::readVarint(body.data(),p,body.size()); else if (wt==1) p+=8; else if (wt==2) { uint64_t l=SteamProto::readVarint(body.data(),p,body.size()); p+=l; } else if (wt==5) p+=4; else p++; }
    }
    NSData *subData = [NSData dataWithBytes:msgBody.data() length:msgBody.size()];
    if (sizeUnzipped > 0) {
        NSData *dec = gzipDecompress(subData);
        if (dec) subData = dec;
    }
    NSMutableArray *msgs = [NSMutableArray array];
    const uint8_t *dp = (const uint8_t *)subData.bytes;
    size_t dl = subData.length, off = 0;
    while (off + 4 <= dl) {
        int subSize = readInt32LE(dp, off); off += 4;
        if (subSize <= 0 || off + (size_t)subSize > dl) break;
        int subEmsg = readInt32LE(dp, off) & 0x7FFFFFFF;
        int hdrLen = readInt32LE(dp, off + 4);
        size_t subOff = 8 + hdrLen;
        if (subOff < (size_t)subSize) {
            bytes subHeader(dp + off + 8, dp + off + 8 + hdrLen);
            bytes subBody(dp + off + subOff, dp + off + subSize);
            [msgs addObject:@{@"emsg":@(subEmsg), @"header":[NSData dataWithBytes:subHeader.data() length:subHeader.size()], @"body":[NSData dataWithBytes:subBody.data() length:subBody.size()]}];
        }
        off += subSize;
    }
    return msgs;
}

- (BOOL)readMessageWithEmsg:(int*)oe body:(bytes*)ob isProto:(BOOL*)op header:(bytes*)oh error:(NSError**)err {
    uint8_t lb[4]; if (![self readExact:lb length:4]) { logToFile(@"readMessage: failed reading len"); if (err) *err = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Closed reading len"}]; return NO; }
    int32_t pl = readInt32LE(lb, 0);
    logToFile(@"readMessage: packetLen=%d encrypted=%d", pl, _encrypted);
    uint8_t mb[4]; if (![self readExact:mb length:4]) { logToFile(@"readMessage: failed reading magic after len=%d", pl); if (err) *err = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Closed reading magic"}]; return NO; }
    int32_t magic = readInt32LE(mb,0);
    if (magic != TCP_MAGIC) { logToFile(@"readMessage: BAD MAGIC 0x%08x (expected 0x%08x)", magic, TCP_MAGIC); if (err) *err = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Bad magic"}]; return NO; }
    if (pl < 4 || pl > 1000000) { logToFile(@"readMessage: BAD LEN %d", pl); if (err) *err = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"Bad len %d",pl]}]; return NO; }
    bytes ep((size_t)pl); if (![self readExact:ep.data() length:(size_t)pl]) { logToFile(@"readMessage: failed reading %d byte payload", pl); if (err) *err = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Closed reading payload"}]; return NO; }
    bytes payload;
    if (_encrypted) { try { logToFile(@"readMessage: decrypting %zu bytes...", ep.size()); payload = decryptAES(_sessionKey, ep.data(), ep.size()); logToFile(@"readMessage: decrypted to %zu bytes", payload.size()); } catch (...) { logToFile(@"readMessage: DECRYPT FAILED"); if (err) *err = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Decrypt failed"}]; return NO; } }
    else payload = ep;
    if (payload.size() < 8) { logToFile(@"readMessage: payload too small %zu", payload.size()); if (err) *err = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Payload too small"}]; return NO; }
    int re = readInt32LE(payload.data(), 0); BOOL ip = (re & PROTO_MASK) != 0;
    size_t off = 4;
    if (ip) { int hl = readInt32LE(payload.data(), (size_t)off); off += 4; logToFile(@"readMessage: proto emsg=%d headerLen=%d", re & 0x7FFFFFFF, hl); if (oh) oh->assign(payload.begin()+off, payload.begin()+off+hl); if (hl > 0) off += hl; } else { logToFile(@"readMessage: legacy emsg=%d", re & 0x7FFFFFFF); off += 16; }
    if (oe) *oe = re & 0x7FFFFFFF; if (op) *op = ip;
    if (ob) ob->assign(payload.begin()+off, payload.end());
    logToFile(@"readMessage: body=%zu", ob ? ob->size() : 0);
    return YES;
}

- (BOOL)anonymousLoginWithError:(NSError **)error {
    arc4random_buf(_sessionKey, 32); _encrypted = NO;
    [self status:@"Waiting for encrypt request..."];
    logToFile(@"Waiting for ChannelEncryptRequest...");
    int emsg; bytes body; BOOL isProto;
    if (![self readMessageWithEmsg:&emsg body:&body isProto:&isProto header:nil error:error]) {
        logToFile(@"Failed to read ChannelEncryptRequest");
        return NO;
    }
    logToFile(@"Got msg: emsg=%d isProto=%d bodySize=%zu", emsg, isProto, body.size());

    NSData *pubKey; bytes challenge;
    if (isProto) {
        size_t p = 0;
        skipProtoField(body.data(), p, body.size()); // protocol_version
        skipProtoField(body.data(), p, body.size()); // universe
        uint64_t ftag = SteamProto::readVarint(body.data(), p, body.size());
        int fn = (int)(ftag >> 3);
        int wt = (int)(ftag & 7);
        logToFile(@"Proto field 3: tag=0x%llx fn=%d wt=%d pos=%zu size=%zu", (unsigned long long)ftag, fn, wt, p, body.size());
        if (wt == 2) {
            uint64_t kl = SteamProto::readVarint(body.data(), p, body.size());
            logToFile(@"  -> bytes field, keyLen=%llu", (unsigned long long)kl);
            if (kl > 0 && p + kl <= body.size()) {
                pubKey = [NSData dataWithBytes:body.data()+p length:(NSUInteger)kl];
                p += (size_t)kl;
            }
        } else if (wt == 0) {
            uint64_t kl = SteamProto::readVarint(body.data(), p, body.size());
            logToFile(@"  -> varint field, value=%llu", (unsigned long long)kl);
            if (kl > 0 && p + (size_t)kl <= body.size()) {
                pubKey = [NSData dataWithBytes:body.data()+p length:(NSUInteger)kl];
                p += (size_t)kl;
            }
        }
        logToFile(@"  pubKey size=%lu pos=%zu", (unsigned long)pubKey.length, p);
        // Field 4 = challenge (bytes). Match Android: read ALL remaining bytes
        // (including protobuf tag + length framing) to match server expectations
        if (p < body.size()) {
            challenge.assign(body.begin()+p, body.end());
        }
        logToFile(@"  challenge size=%zu", challenge.size());
    } else {
        pubKey = steamPublicKey();
        size_t off = 8; if (off < body.size()) challenge = bytes(body.begin()+off, body.end());
        logToFile(@"Legacy format: pubKey=%lu challenge=%zu", (unsigned long)pubKey.length, challenge.size());
    }

    NSMutableData *blobToEncrypt = [NSMutableData dataWithBytes:_sessionKey length:32];
    [blobToEncrypt appendBytes:challenge.data() length:challenge.size()];
    logToFile(@"blobToEncrypt size=%lu", (unsigned long)blobToEncrypt.length);

    NSData *eb = rsaEncryptOAEP((const uint8_t *)pubKey.bytes, pubKey.length, (const uint8_t *)blobToEncrypt.bytes, blobToEncrypt.length);
    if (!eb) {
        logToFile(@"RSA encrypt failed with server key, trying hardcoded key");
        pubKey = steamPublicKey();
        eb = rsaEncryptOAEP((const uint8_t *)pubKey.bytes, pubKey.length, (const uint8_t *)blobToEncrypt.bytes, blobToEncrypt.length);
    }
    if (!eb) {
        logToFile(@"RSA encrypt failed with hardcoded key too!");
        if (error) *error = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"RSA failed"}];
        return NO;
    }
    logToFile(@"RSA encrypt success, encrypted size=%lu", (unsigned long)eb.length);

    NSData *bc = eb;
    uint32_t crc = (uint32_t)crc32(0, (const uint8_t *)eb.bytes, (uInt)eb.length);
    bytes er; auto r1 = packFixed32(1304); er.insert(er.end(), r1.begin(), r1.end());
    auto r2 = packFixed64(-1); er.insert(er.end(), r2.begin(), r2.end());
    auto r3 = packFixed64(-1); er.insert(er.end(), r3.begin(), r3.end());
    auto b1 = packFixed32(1); er.insert(er.end(), b1.begin(), b1.end());
    auto b2 = packFixed32((int32_t)bc.length); er.insert(er.end(), b2.begin(), b2.end());
    er.insert(er.end(), (const uint8_t *)bc.bytes, (const uint8_t *)bc.bytes+bc.length);
    auto cf = packFixed32((int32_t)crc); er.insert(er.end(), cf.begin(), cf.end());
    auto zf = packFixed32(0); er.insert(er.end(), zf.begin(), zf.end());
    [self sendRaw:er]; [self status:@"Sent encrypt response"];

    if (![self readMessageWithEmsg:&emsg body:&body isProto:&isProto header:nil error:error]) {
        logToFile(@"Failed to read ChannelEncryptResult");
        return NO;
    }
    logToFile(@"ChannelEncryptResult: emsg=%d isProto=%d bodySize=%zu", emsg, isProto, body.size());
    // Parse eresult — handle both proto (varint) and non-proto (LE int32) formats
    int eresult = 0;
    if (isProto && body.size() > 0) {
        size_t ep = 0;
        SteamProto::readVarint(body.data(), ep, body.size()); // skip field tag
        eresult = (int)SteamProto::readVarint(body.data(), ep, body.size());
    } else if (body.size() >= 4) {
        eresult = readInt32LE(body.data(), 0);
    }
    logToFile(@"Encrypt result: %d", eresult);
    if (eresult != 1) { if (error) *error = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"Handshake failed: eresult=%d",eresult]}]; return NO; }
    _encrypted = YES; [self status:@"Connecting Steam..."];
    logToFile(@"Encryption established, sending ClientHello...");

    // Build proto header helper
    auto buildHeader = [&](uint64_t steamId, int sessionId) -> bytes {
        bytes h;
        auto t1 = packVarint((1<<3)|1); auto v1 = packFixed64(steamId);
        h.insert(h.end(), t1.begin(), t1.end()); h.insert(h.end(), v1.begin(), v1.end());
        auto t2 = packVarint((2<<3)|0); auto v2 = packVarint(sessionId);
        h.insert(h.end(), t2.begin(), t2.end()); h.insert(h.end(), v2.begin(), v2.end());
        return h;
    };

    // ClientHello (9805) — MUST include header with steamId like Android does
    bytes hb; auto hf = packVarint((1<<3)|0); hb.insert(hb.end(),hf.begin(),hf.end());
    auto hv = packVarint(65581); hb.insert(hb.end(),hv.begin(),hv.end());
    bytes helloHeader = buildHeader(_currentSteamId, 0);
    logToFile(@"Sending ClientHello (emsg=9805) header=%zu body=%zu", helloHeader.size(), hb.size());
    [self sendProtobufMsg:9805 body:hb header:helloHeader];
    usleep(100000);

    // ClientLogon (5514)
    bytes lb; auto lf = packVarint((1<<3)|0); lb.insert(lb.end(),lf.begin(),lf.end());
    auto lv = packVarint(65581); lb.insert(lb.end(),lv.begin(),lv.end());
    auto lf6 = packVarint((6<<3)|2); bytes lang={'e','n','g','l','i','s','h'};
    auto ll = packVarint((int64_t)lang.size()); lb.insert(lb.end(),lf6.begin(),lf6.end());
    lb.insert(lb.end(),ll.begin(),ll.end()); lb.insert(lb.end(),lang.begin(),lang.end());
    auto lf7 = packVarint((7<<3)|0); auto lv7 = packVarint((int64_t)-500);
    lb.insert(lb.end(),lf7.begin(),lf7.end()); lb.insert(lb.end(),lv7.begin(),lv7.end());
    auto lf30 = packVarint((30<<3)|2); bytes mid={'J','a','v','a','S','t','e','a','m','-','S','e','r','i','a','l','N','u','m','b','e','r'};
    auto ml = packVarint((int64_t)mid.size()); lb.insert(lb.end(),lf30.begin(),lf30.end());
    lb.insert(lb.end(),ml.begin(),ml.end()); lb.insert(lb.end(),mid.begin(),mid.end());
    bytes lh = buildHeader(_currentSteamId, 0);
    logToFile(@"Sending ClientLogon (emsg=5514) header=%zu body=%zu", lh.size(), lb.size());
    [self sendProtobufMsg:5514 body:lb header:lh];

    BOOL ok = NO;
    for (int t = 0; t < 10 && !ok; t++) {
        bytes hdr;
        if (![self readMessageWithEmsg:&emsg body:&body isProto:&isProto header:&hdr error:nil]) {
            logToFile(@"Login response read failed (try %d)", t);
            usleep(100000); continue;
        }
        logToFile(@"Login response: emsg=%d isProto=%d bodySize=%zu (try %d)", emsg, isProto, body.size(), t);
        if (emsg == 1) {
            NSArray *subs = [self parseMultiMessage:body];
            for (NSDictionary *sub in subs) {
                int se = [sub[@"emsg"] intValue];
                NSData *sd = sub[@"body"];
                NSData *sh = sub[@"header"];
                logToFile(@"  Sub-msg: emsg=%d size=%lu", se, (unsigned long)sd.length);
                if (se == 751) {
                    if (sh) [self parseLogonResponseHeader:sh];
                    const uint8_t *sp = (const uint8_t *)sd.bytes;
                    size_t sl = sd.length, spp = 0;
                    while (spp < sl) {
                        uint64_t tag = SteamProto::readVarint(sp, spp, sl);
                        int fn = (int)(tag>>3), wt = (int)(tag&7);
                        if (fn == 1 && wt == 0) {
                            int lr = (int)SteamProto::readVarint(sp, spp, sl);
                            if (lr == 1) ok = YES;
                        } else if (fn == 2 && wt == 0) {
                            _heartbeatSeconds = (int)SteamProto::readVarint(sp, spp, sl);
                            logToFile(@"  heartbeat_seconds=%d", _heartbeatSeconds);
                        } else {
                            if (wt == 0) SteamProto::readVarint(sp, spp, sl);
                            else if (wt == 1) spp += 8;
                            else if (wt == 2) { uint64_t l = SteamProto::readVarint(sp, spp, sl); spp += (size_t)l; }
                            else if (wt == 5) spp += 4;
                            else spp++;
                        }
                    }
                }
            }
        } else if (emsg == 751) {
            NSData *hdrData = [NSData dataWithBytes:hdr.data() length:hdr.size()];
            [self parseLogonResponseHeader:hdrData];
            size_t sp = 0;
            while (sp < body.size()) {
                uint64_t tag = SteamProto::readVarint(body.data(), sp, body.size());
                int fn = (int)(tag>>3), wt = (int)(tag&7);
                if (fn == 1 && wt == 0) {
                    int lr = (int)SteamProto::readVarint(body.data(), sp, body.size());
                    logToFile(@"  Direct logon: eresult=%d sessionId=%d steamId=%llu", lr, _currentSessionId, (unsigned long long)_currentSteamId);
                    if (lr == 1) ok = YES;
                } else if (fn == 2 && wt == 0) {
                    _heartbeatSeconds = (int)SteamProto::readVarint(body.data(), sp, body.size());
                    logToFile(@"  heartbeat_seconds=%d", _heartbeatSeconds);
                } else {
                    if (wt == 0) SteamProto::readVarint(body.data(), sp, body.size());
                    else if (wt == 1) sp += 8;
                    else if (wt == 2) { uint64_t l = SteamProto::readVarint(body.data(), sp, body.size()); sp += (size_t)l; }
                    else if (wt == 5) sp += 4;
                    else sp++;
                }
            }
        } else if (emsg == 757) {
            logToFile(@"  ServiceMethodResponse (emsg 757) — login rejected");
            break;
        } else {
            logToFile(@"  Unexpected emsg=%d in login loop", emsg);
        }
    }
    if (!ok) { if (error) *error = [NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Login failed"}]; return NO; }
    [self status:@"Logged in to Steam"];
    logToFile(@"Login success! sessionId=%d steamId=%llu", _currentSessionId, (unsigned long long)_currentSteamId);
    [self startHeartbeat:_heartbeatSeconds];
    return YES;
}

- (void)parseLogonResponseHeader:(NSData *)header {
    const uint8_t *b = (const uint8_t *)header.bytes;
    size_t l = header.length, p = 0;
    while (p < l) {
        uint64_t tag = SteamProto::readVarint(b, p, l);
        int fn = (int)(tag >> 3), wt = (int)(tag & 7);
        if (fn == 1 && wt == 1 && p + 8 <= l) {
            _currentSteamId = (uint64_t)readInt64LE(b, p);
            p += 8;
        } else if (fn == 2) {
            if (wt == 0) _currentSessionId = (int)SteamProto::readVarint(b, p, l);
            else if (wt == 5 && p + 4 <= l) { _currentSessionId = readInt32LE(b, p); p += 4; }
            else { if (wt == 0) SteamProto::readVarint(b, p, l); else if (wt == 1) p += 8; else if (wt == 2) { uint64_t sl = SteamProto::readVarint(b, p, l); p += (size_t)sl; } else if (wt == 5) p += 4; else p++; }
        } else {
            if (wt == 0) SteamProto::readVarint(b, p, l);
            else if (wt == 1) p += 8;
            else if (wt == 2) { uint64_t sl = SteamProto::readVarint(b, p, l); p += (size_t)sl; }
            else if (wt == 5) p += 4;
            else p++;
        }
    }
}

- (BOOL)requestLicense:(int)appId error:(NSError **)error {
    bytes b; auto f=packVarint((2<<3)|0); auto v=packVarint(appId);
    b.insert(b.end(),f.begin(),f.end()); b.insert(b.end(),v.begin(),v.end());
    bytes h; auto h1=packVarint((1<<3)|1); auto si=packFixed64(_currentSteamId);
    h.insert(h.end(),h1.begin(),h1.end()); h.insert(h.end(),si.begin(),si.end());
    auto h2=packVarint((2<<3)|0); auto se=packVarint32(_currentSessionId);
    h.insert(h.end(),h2.begin(),h2.end()); h.insert(h.end(),se.begin(),se.end());
    [self sendProtobufMsg:5572 body:b header:h];
    for (int t=0;t<3;t++) {
        int e; bytes d; BOOL ip;
        if (![self readMessageWithEmsg:&e body:&d isProto:&ip header:nil error:nil]) continue;
        if (e == 1) {
            NSArray *subs = [self parseMultiMessage:d];
            for (NSDictionary *sub in subs) {
                if ([sub[@"emsg"] intValue] == 5573) {
                    NSData *sd = sub[@"body"];
                    const uint8_t *sp = (const uint8_t *)sd.bytes; size_t sl = sd.length, spp = 0;
                    SteamProto::readVarint(sp, spp, sl); SteamProto::readVarint(sp, spp, sl); SteamProto::readVarint(sp, spp, sl);
                    int g=(int)SteamProto::readVarint(sp, spp, sl);
                    if (g==appId) return YES;
                }
            }
        } else if (e == 5573) {
            size_t sp=0; SteamProto::readVarint(d.data(),sp,d.size());
            SteamProto::readVarint(d.data(),sp,d.size()); SteamProto::readVarint(d.data(),sp,d.size());
            int g=(int)SteamProto::readVarint(d.data(),sp,d.size()); if (g==appId) return YES;
        } else if (e == 757) return NO;
    }
    return NO;
}

- (NSArray *)requestCDNServerListWithError:(NSError **)error {
    bytes b; auto f1=packVarint((1<<3)|0); b.insert(b.end(),f1.begin(),f1.end()); auto v1=packVarint(1); b.insert(b.end(),v1.begin(),v1.end());
    auto f2=packVarint((2<<3)|0); b.insert(b.end(),f2.begin(),f2.end()); auto v2=packVarint(5); b.insert(b.end(),v2.begin(),v2.end());
    bytes h; auto h1=packVarint((1<<3)|1); auto si=packFixed64(_currentSteamId);
    h.insert(h.end(),h1.begin(),h1.end()); h.insert(h.end(),si.begin(),si.end());
    auto h2=packVarint((2<<3)|0); auto se=packVarint32(_currentSessionId);
    h.insert(h.end(),h2.begin(),h2.end()); h.insert(h.end(),se.begin(),se.end());
    auto h10=packVarint((10<<3)|1); auto ji=packFixed64(_nextJobId++);
    h.insert(h.end(),h10.begin(),h10.end()); h.insert(h.end(),ji.begin(),ji.end());
    std::string jn="ContentServerDirectory.GetServersForSteamPipe#1";
    auto h12=packVarint((12<<3)|2); auto jl=packVarint((int64_t)jn.size());
    h.insert(h.end(),h12.begin(),h12.end()); h.insert(h.end(),jl.begin(),jl.end()); h.insert(h.end(),jn.begin(),jn.end());
    [self sendProtobufMsg:151 body:b header:h];
    NSMutableArray *sv = [NSMutableArray array];
    for (int t=0;t<15;t++) {
        int e; bytes d; BOOL ip;
        if (![self readMessageWithEmsg:&e body:&d isProto:&ip header:nil error:nil]) continue;
        if (e == 147) { [self parseServerList:d into:sv]; if (sv.count) return filterResolvableHosts(sv); }
        else if (e == 1) {
            NSArray *subs = [self parseMultiMessage:d];
            for (NSDictionary *sub in subs) {
                if ([sub[@"emsg"] intValue] == 147) {
                    NSData *sd = sub[@"body"];
                    bytes sb((const uint8_t *)sd.bytes, (const uint8_t *)sd.bytes + sd.length);
                    [self parseServerList:sb into:sv];
                }
            }
            if (sv.count) return filterResolvableHosts(sv);
        } else if (e == 757) break;
    }
    if (error) *error=[NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"No CDN servers"}];
    return nil;
}

- (void)parseServerList:(const bytes &)d into:(NSMutableArray *)sv {
    size_t p=0;
    while (p<d.size()) {
        uint64_t tg=SteamProto::readVarint(d.data(),p,d.size()); int fn=(int)(tg>>3), wt=(int)(tg&7);
        if (fn==1&&wt==2) {
            uint64_t sl=SteamProto::readVarint(d.data(),p,d.size()); size_t e=p+(size_t)sl;
            NSString *host=nil, *vhost=nil;
            while (p<e) {
                uint64_t ft=SteamProto::readVarint(d.data(),p,d.size()); int ffn=(int)(ft>>3), fwt=(int)(ft&7);
                if (ffn==8&&fwt==2) { uint64_t hl=SteamProto::readVarint(d.data(),p,d.size());
                    host=[[NSString alloc] initWithBytes:d.data()+p length:(NSUInteger)hl encoding:NSUTF8StringEncoding]; p+=hl; }
                else if (ffn==9&&fwt==2) { uint64_t hl=SteamProto::readVarint(d.data(),p,d.size());
                    vhost=[[NSString alloc] initWithBytes:d.data()+p length:(NSUInteger)hl encoding:NSUTF8StringEncoding]; p+=hl; }
                else { if (fwt==0) SteamProto::readVarint(d.data(),p,d.size()); else if (fwt==1) p+=8; else if (fwt==2) { uint64_t l=SteamProto::readVarint(d.data(),p,d.size()); p+=l; } else if (fwt==5) p+=4; else p++; }
            }
            NSString *uh=vhost?:host; if (uh.length>0) [sv addObject:uh];
        } else { if (wt==0) SteamProto::readVarint(d.data(),p,d.size()); else if (wt==1) p+=8; else if (wt==2) { uint64_t l=SteamProto::readVarint(d.data(),p,d.size()); p+=l; } else if (wt==5) p+=4; else p++; }
    }
}

- (NSData *)getDepotDecryptionKey:(int)appId depotId:(int)depotId error:(NSError **)error {
    bytes b; auto f1=packVarint((1<<3)|0); auto v1=packVarint(depotId);
    b.insert(b.end(),f1.begin(),f1.end()); b.insert(b.end(),v1.begin(),v1.end());
    auto f2=packVarint((2<<3)|0); auto v2=packVarint(appId);
    b.insert(b.end(),f2.begin(),f2.end()); b.insert(b.end(),v2.begin(),v2.end());
    bytes h; auto h1=packVarint((1<<3)|1); auto si=packFixed64(_currentSteamId);
    h.insert(h.end(),h1.begin(),h1.end()); h.insert(h.end(),si.begin(),si.end());
    auto h2=packVarint((2<<3)|0); auto se=packVarint32(_currentSessionId);
    h.insert(h.end(),h2.begin(),h2.end()); h.insert(h.end(),se.begin(),se.end());
    [self sendProtobufMsg:5438 body:b header:h];
    logToFile(@"[depotKey] Sent request for depot %d", depotId);
    for (int t=0;t<15;t++) {
        int e; bytes d; BOOL ip;
        if (![self readMessageWithEmsg:&e body:&d isProto:&ip header:nil error:nil]) { logToFile(@"[depotKey] readMessage failed try %d", t); continue; }
        logToFile(@"[depotKey] Got emsg=%d body=%zu try %d", e, d.size(), t);
        if (e==1) {
            NSArray *subs = [self parseMultiMessage:d];
            logToFile(@"[depotKey] Multi has %lu sub-messages", (unsigned long)subs.count);
            for (NSDictionary *sub in subs) {
                int subEmsg = [sub[@"emsg"] intValue];
                logToFile(@"[depotKey] Sub-msg emsg=%d size=%lu", subEmsg, (unsigned long)[sub[@"body"] length]);
                if (subEmsg == 5439) {
                    NSData *sd = sub[@"body"];
                    const uint8_t *sp=(const uint8_t *)sd.bytes; size_t sl=sd.length, spp=0;
                    skipProtoField(sp, spp, sl);
                    skipProtoField(sp, spp, sl);
                    SteamProto::readVarint(sp, spp, sl);
                    int kl=(int)SteamProto::readVarint(sp, spp, sl);
                    logToFile(@"[depotKey] Parsed keyLength=%d remaining=%zu", kl, sl-spp);
                    if (kl>0 && (size_t)kl<=sl-spp) {
                        logToFile(@"[depotKey] Returning key of %d bytes", kl);
                        return [NSData dataWithBytes:sp+spp length:kl];
                    }
                }
            }
        } else if (e==5439) {
            size_t sp=0;
            skipProtoField(d.data(), sp, d.size());
            skipProtoField(d.data(), sp, d.size());
            SteamProto::readVarint(d.data(), sp, d.size());
            int kl=(int)SteamProto::readVarint(d.data(), sp, d.size());
            logToFile(@"[depotKey] Direct 5439: keyLength=%d remaining=%zu", kl, d.size()-sp);
            if (kl>0&&(size_t)kl<=d.size()-sp) {
                logToFile(@"[depotKey] Returning key of %d bytes", kl);
                return [NSData dataWithBytes:d.data()+sp length:kl];
            }
        } else if (e==757) { logToFile(@"[depotKey] Got logged off"); break; }
    }
    if (error) *error=[NSError errorWithDomain:@"SteamCM" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"No depot key"}];
    return nil;
}

- (uint64_t)requestManifestRequestCode:(int)depotId appId:(int)appId manifestId:(int64_t)manifestId error:(NSError **)error {
    bytes b; auto f1=packVarint((1<<3)|0); auto v1=packVarint(appId);
    b.insert(b.end(),f1.begin(),f1.end()); b.insert(b.end(),v1.begin(),v1.end());
    auto f2=packVarint((2<<3)|0); auto v2=packVarint(depotId);
    b.insert(b.end(),f2.begin(),f2.end()); b.insert(b.end(),v2.begin(),v2.end());
    auto f3=packVarint((3<<3)|0); auto v3=packVarint64((uint64_t)manifestId);
    b.insert(b.end(),f3.begin(),f3.end()); b.insert(b.end(),v3.begin(),v3.end());
    bytes h; auto h1=packVarint((1<<3)|1); auto si=packFixed64(_currentSteamId);
    h.insert(h.end(),h1.begin(),h1.end()); h.insert(h.end(),si.begin(),si.end());
    auto h2=packVarint((2<<3)|0); auto se=packVarint32(_currentSessionId);
    h.insert(h.end(),h2.begin(),h2.end()); h.insert(h.end(),se.begin(),se.end());
    auto h10=packVarint((10<<3)|1); auto ji=packFixed64(_nextJobId++);
    h.insert(h.end(),h10.begin(),h10.end()); h.insert(h.end(),ji.begin(),ji.end());
    std::string jn="ContentServerDirectory.GetManifestRequestCode#1";
    auto h12=packVarint((12<<3)|2); auto jl=packVarint((int64_t)jn.size());
    h.insert(h.end(),h12.begin(),h12.end()); h.insert(h.end(),jl.begin(),jl.end()); h.insert(h.end(),jn.begin(),jn.end());
    [self sendProtobufMsg:151 body:b header:h];
    for (int t=0;t<15;t++) {
        int e; bytes d; BOOL ip;
        if (![self readMessageWithEmsg:&e body:&d isProto:&ip header:nil error:nil]) continue;
        if (e==147) {
            size_t sp=0;
            while (sp < d.size()) {
                uint64_t tag = SteamProto::readVarint(d.data(), sp, d.size());
                int fn = (int)(tag>>3), wt = (int)(tag&7);
                if (fn == 1 && wt == 0) return SteamProto::readVarint(d.data(), sp, d.size());
                else { if (wt == 0) SteamProto::readVarint(d.data(), sp, d.size()); else if (wt == 1) sp += 8; else if (wt == 2) { uint64_t l = SteamProto::readVarint(d.data(), sp, d.size()); sp += (size_t)l; } else if (wt == 5) sp += 4; else sp++; }
            }
        } else if (e==1) {
            NSArray *subs = [self parseMultiMessage:d];
            for (NSDictionary *sub in subs) {
                if ([sub[@"emsg"] intValue] == 147) {
                    NSData *sd = sub[@"body"];
                    const uint8_t *sp = (const uint8_t *)sd.bytes; size_t sl = sd.length, spp = 0;
                    while (spp < sl) {
                        uint64_t tag = SteamProto::readVarint(sp, spp, sl);
                        int fn = (int)(tag>>3), wt = (int)(tag&7);
                        if (fn == 1 && wt == 0) return SteamProto::readVarint(sp, spp, sl);
                        else { if (wt == 0) SteamProto::readVarint(sp, spp, sl); else if (wt == 1) spp += 8; else if (wt == 2) { uint64_t l = SteamProto::readVarint(sp, spp, sl); spp += (size_t)l; } else if (wt == 5) spp += 4; else spp++; }
                    }
                }
            }
        } else if (e==757) break;
    }
    return 0;
}

@end

#pragma mark - Manifest download & parsing

static NSData *downloadManifestFromCDN(int depotId, int64_t manifestId, uint64_t rc, NSArray *hosts) {
    for (int hi=0;;hi++) {
        NSString *host = hosts[hi % hosts.count];
        NSString *path = rc>0 ? [NSString stringWithFormat:@"depot/%d/manifest/%lld/5/%llu",depotId,(long long)manifestId,(unsigned long long)rc]
                               : [NSString stringWithFormat:@"depot/%d/manifest/%lld/5",depotId,(long long)manifestId];
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"https://%@/%@",host,path]]];
        [req setValue:@"Valve/Steam HTTP Client 1.0" forHTTPHeaderField:@"User-Agent"]; req.timeoutInterval=15;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSData *res=nil;
        __block int statusCode = 0;
        [[NSURLSession.sharedSession dataTaskWithRequest:req completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
            if (e) logToFile(@"[cdn-manifest] depot %d host %@ error: %@", depotId, host, e.localizedDescription);
            statusCode = (int)((NSHTTPURLResponse *)r).statusCode;
            if (statusCode == 200 && d.length>0) res = unzipFirstEntry(d);
            dispatch_semaphore_signal(sem);
        }] resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        if (res) { logToFile(@"[cdn-manifest] depot %d host %@ OK (%d bytes)", depotId, host, (int)res.length); return res; }
        logToFile(@"[cdn-manifest] depot %d host %@ FAILED HTTP %d (try %d)", depotId, host, statusCode, hi);
        usleep(500000);
        if (hi > (int)hosts.count * 3) break;
    }
    logToFile(@"[cdn-manifest] depot %d ALL HOSTS EXHAUSTED", depotId);
    return nil;
}

static NSArray *parseManifestFiles(NSData *data, NSData *depotKey) {
    const uint8_t *b = (const uint8_t *)data.bytes; size_t l = data.length, p = 0;
    NSData *payload = nil;
    while (p+4 <= l) {
        uint32_t magic = (uint32_t)readInt32LE(b, p); p += 4;
        if (p+4 > l) break;
        uint32_t plen = (uint32_t)readInt32LE(b, p); p += 4;
        if (magic == 0x71F617D0) { if (p+plen <= l) payload = [NSData dataWithBytes:b+p length:plen]; p += plen; }
        else if (magic == 0x1F4812BE || magic == 0x1B81B817) p += plen;
        else if (magic == 0x32C415AB) break;
        else break;
    }
    if (!payload) return @[];

    const uint8_t *pb = (const uint8_t *)payload.bytes; size_t pl = payload.length, pp = 0;
    NSMutableArray *files = [NSMutableArray array];
    while (pp < pl) {
        uint64_t tag = SteamProto::readVarint(pb, pp, pl);
        int fn = (int)(tag>>3), wt = (int)(tag&7);
        if (fn == 1 && wt == 2) {
            uint64_t fl = SteamProto::readVarint(pb, pp, pl); size_t end = pp + (size_t)fl;
            int64_t size = 0; int crc32 = 0; NSData *rawFn = nil;
            NSMutableArray *chunks = [NSMutableArray array];
            while (pp < end) {
                uint64_t ft = SteamProto::readVarint(pb, pp, pl);
                int ffn = (int)(ft>>3), fwt = (int)(ft&7);
                if (ffn == 1 && fwt == 2) { uint64_t sl = SteamProto::readVarint(pb, pp, pl);
                    rawFn = [NSData dataWithBytes:pb+pp length:(NSUInteger)sl]; pp += sl; }
                else if (ffn == 2 && fwt == 0) size = (int64_t)SteamProto::readVarint(pb, pp, pl);
                else if (ffn == 3 && fwt == 0) crc32 = (int)SteamProto::readVarint(pb, pp, pl);
                else if (ffn == 6 && fwt == 2) {
                    uint64_t cl = SteamProto::readVarint(pb, pp, pl); size_t ce = pp + (size_t)cl;
                    NSData *sha1 = nil; int ck = 0; int64_t off = 0; int co = 0, cu = 0;
                    while (pp < ce) {
                        uint64_t ct = SteamProto::readVarint(pb, pp, pl);
                        int cfn = (int)(ct>>3), cwt = (int)(ct&7);
                        if (cfn == 1 && cwt == 2) { uint64_t sl = SteamProto::readVarint(pb, pp, pl);
                            sha1 = [NSData dataWithBytes:pb+pp length:(NSUInteger)sl]; pp += sl; }
                        else if (cfn == 2 && cwt == 5) { ck = readInt32LE(pb, pp); pp += 4; }
                        else if (cfn == 3 && cwt == 0) off = (int64_t)SteamProto::readVarint(pb, pp, pl);
                        else if (cfn == 4 && cwt == 0) cu = (int)SteamProto::readVarint(pb, pp, pl);
                        else if (cfn == 5 && cwt == 0) co = (int)SteamProto::readVarint(pb, pp, pl);
                        else { if (cwt==0) SteamProto::readVarint(pb,pp,pl); else if (cwt==1) pp+=8; else if (cwt==2) { uint64_t l2=SteamProto::readVarint(pb,pp,pl); pp+=l2; } else if (cwt==5) pp+=4; else pp++; }
                    }
                    if (sha1) [chunks addObject:@{@"sha1":sha1, @"checksum":@(ck), @"offset":@(off), @"compressedLength":@(co), @"uncompressedLength":@(cu)}];
                } else { if (fwt==0) SteamProto::readVarint(pb,pp,pl); else if (fwt==1) pp+=8; else if (fwt==2) { uint64_t l2=SteamProto::readVarint(pb,pp,pl); pp+=l2; } else if (fwt==5) pp+=4; else pp++; }
            }
            NSString *path = @"";
            if (rawFn) path = [decryptFilename((const uint8_t *)rawFn.bytes, rawFn.length, depotKey) stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
            if (path.length > 0) [files addObject:@{@"path":path, @"size":@(size), @"crc":@(crc32), @"chunks":chunks}];
        } else { if (wt==0) SteamProto::readVarint(pb,pp,pl); else if (wt==1) pp+=8; else if (wt==2) { uint64_t l2=SteamProto::readVarint(pb,pp,pl); pp+=l2; } else if (wt==5) pp+=4; else pp++; }
    }
    return files;
}

#pragma mark - Chunk download & file assembly

static NSData *downloadChunkAndDecrypt(int depotId, NSDictionary *chunk, NSData *depotKey, NSArray *hosts) {
    NSData *sha1 = chunk[@"sha1"];
    const uint8_t *sp = (const uint8_t *)sha1.bytes;
    NSMutableString *hex = [NSMutableString string];
    for (NSUInteger i = 0; i < sha1.length; i++) [hex appendFormat:@"%02x", sp[i]];

    for (int hi = 0; ; hi++) {
        NSString *host = hosts[hi % hosts.count];
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"https://%@/depot/%d/chunk/%@", host, depotId, hex]]];
        [req setValue:@"Valve/Steam HTTP Client 1.0" forHTTPHeaderField:@"User-Agent"]; req.timeoutInterval = 15;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSData *res = nil;
        [[NSURLSession.sharedSession dataTaskWithRequest:req completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
            NSHTTPURLResponse *http = (NSHTTPURLResponse *)r;
            if (http.statusCode == 200 && d.length >= 16) {
                NSData *dec = decryptChunk((const uint8_t *)depotKey.bytes, depotKey.length, (const uint8_t *)d.bytes, d.length);
                if (dec) {
                    const uint8_t *db = (const uint8_t *)dec.bytes; size_t dl = dec.length;
                    if (dl >= 4 && db[0]=='P' && db[1]=='K' && db[2]==0x03 && db[3]==0x04) {
                        res = unzipFirstEntry(dec);
                    } else if (dl >= 17 && db[0]=='V' && db[1]=='Z' && db[2]=='a') {
                        uint8_t *decomp = nil; size_t dsz = 0;
                        if (vzip_decompress(db, dl, &decomp, &dsz) == 0)
                            res = [NSData dataWithBytesNoCopy:decomp length:dsz freeWhenDone:YES];
                    } else {
                        res = dec;
                    }
                    if (res) {
                        int adler = adler32_steam((const uint8_t *)res.bytes, res.length);
                        if (adler != [chunk[@"checksum"] intValue]) res = nil;
                    }
                }
            }
            dispatch_semaphore_signal(sem);
        }] resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        if (res) return res;
        usleep(500000);
        if (hi > (int)hosts.count * 3) break;
    }
    return nil;
}

static BOOL assembleFile(int depotId, NSDictionary *file, NSString *outPath, NSData *depotKey, NSArray *hosts) {
    NSArray *chunks = file[@"chunks"];
    if (chunks.count == 0) {
        if ([outPath.lastPathComponent containsString:@"."])
            [[NSFileManager defaultManager] createFileAtPath:outPath contents:[NSData data] attributes:nil];
        else
            [[NSFileManager defaultManager] createDirectoryAtPath:outPath withIntermediateDirectories:YES attributes:nil error:nil];
        return YES;
    }
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *p = [outPath stringByDeletingLastPathComponent];
    while (p && ![p isEqualToString:@"/"]) { BOOL isDir=NO;
        if ([fm fileExistsAtPath:p isDirectory:&isDir] && !isDir) { [fm removeItemAtPath:p error:nil]; } p = [p stringByDeletingLastPathComponent]; }
    [fm createDirectoryAtPath:[outPath stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *tmpPath = [[outPath stringByDeletingLastPathComponent] stringByAppendingPathComponent:[[outPath lastPathComponent] stringByAppendingString:@".tmp"]];
    FILE *f = fopen([tmpPath UTF8String], "wb"); if (!f) return NO;
    ftruncate(fileno(f), (off_t)[file[@"size"] longLongValue]);
    __block BOOL ok = YES;
    dispatch_semaphore_t sem = dispatch_semaphore_create(8);
    dispatch_group_t grp = dispatch_group_create();
    for (NSDictionary *ch in chunks) {
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        dispatch_group_enter(grp);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            if (!ok) { dispatch_semaphore_signal(sem); dispatch_group_leave(grp); return; }
            NSData *d = downloadChunkAndDecrypt(depotId, ch, depotKey, hosts);
            if (d) { fseeko(f, (off_t)[ch[@"offset"] longLongValue], SEEK_SET); fwrite(d.bytes, 1, d.length, f); }
            else ok = NO;
            dispatch_semaphore_signal(sem); dispatch_group_leave(grp);
        });
    }
    dispatch_group_wait(grp, DISPATCH_TIME_FOREVER);
    fclose(f);
    if (ok) { [fm removeItemAtPath:outPath error:nil]; [fm moveItemAtPath:tmpPath toPath:outPath error:nil]; return YES; }
    else { [fm removeItemAtPath:tmpPath error:nil]; return NO; }
}

#pragma mark - GameDataDownloader

@implementation GameDataDownloader {
    NSString *_docsDir;
}

- (instancetype)initWithDocumentsDir:(NSString *)docsDir {
    if (self = [super init]) { _docsDir = [docsDir copy]; initLogPath(_docsDir); }
    return self;
}

- (void)downloadGame:(NSString *)gameDir onProgress:(void(^)(NSString*,float))onProgress completion:(void(^)(NSError*))completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSDictionary *info = GAMES()[gameDir];
        if (!info) {
            dispatch_async(dispatch_get_main_queue(), ^{
                completion([NSError errorWithDomain:@"GameDataDownloader" code:-1 userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"Unknown game: %@",gameDir]}]);
            }); return;
        }
        int appId = [info[@"appId"] intValue];
        NSArray *depotIds = info[@"depotIds"];
        NSString *displayName = info[@"displayName"];
        NSString *targetDir = [_docsDir stringByAppendingPathComponent:gameDir];

        SteamCMClient *client = [[SteamCMClient alloc] initWithOnStatus:^(NSString *m) { if (onProgress) onProgress(m, 0); }];

        NSError *err = nil;
        if (onProgress) onProgress(@"Connecting to Steam...", 0);
        logToFile(@"Connecting to Steam CM...");
        if (![client connectWithError:&err]) {
            logToFile(@"Connect failed: %@", err.localizedDescription);
            dispatch_async(dispatch_get_main_queue(), ^{ completion(err); });
            return;
        }

        if (onProgress) onProgress(@"Logging in anonymously...", 0);
        logToFile(@"Logging in anonymously for game %@ (appId=%d)", gameDir, appId);
        if (![client anonymousLoginWithError:&err]) {
            logToFile(@"Login failed: %@", err.localizedDescription);
            [client disconnect];
            dispatch_async(dispatch_get_main_queue(), ^{ completion(err); });
            return;
        }

        if (onProgress) onProgress(@"Getting CDN servers...", 0);
        logToFile(@"Requesting CDN server list...");
        NSArray *cdnHosts = [client requestCDNServerListWithError:&err];
        if (!cdnHosts) {
            logToFile(@"CDN servers failed: %@", err.localizedDescription);
            [client disconnect];
            dispatch_async(dispatch_get_main_queue(), ^{ completion(err); });
            return;
        }
        logToFile(@"Got %lu CDN hosts", (unsigned long)cdnHosts.count);

        NSMutableArray *depotSetups = [NSMutableArray array];
        for (NSNumber *did in depotIds) {
            int depotId = [did intValue];
            NSNumber *mid = HARDCODED_MANIFEST_IDS()[did];
            if (!mid || [mid unsignedLongLongValue] == 0) { logToFile(@"[setup] No manifest ID for depot %d", depotId); continue; }
            logToFile(@"[setup] Getting depot key for depot %d...", depotId);
            NSData *dk = [client getDepotDecryptionKey:appId depotId:depotId error:nil];
            if (!dk) { logToFile(@"[setup] Depot key FAILED for depot %d", depotId); continue; }
            logToFile(@"[setup] Got depot key: %lu bytes", (unsigned long)dk.length);
            logToFile(@"[setup] Requesting manifest request code for depot %d...", depotId);
            uint64_t rc = [client requestManifestRequestCode:depotId appId:appId manifestId:(int64_t)[mid unsignedLongLongValue] error:nil];
            if (rc == 0) { logToFile(@"[setup] Manifest request code is 0 for depot %d", depotId); continue; }
            logToFile(@"[setup] Got manifest request code: %llu for depot %d", (unsigned long long)rc, depotId);
            [depotSetups addObject:@{@"depotId":@(depotId), @"depotKey":dk, @"requestCode":@((unsigned long long)rc)}];
        }
        [client disconnect];
        logToFile(@"[setup] depotSetups count: %lu", (unsigned long)depotSetups.count);

        if (depotSetups.count == 0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                completion([NSError errorWithDomain:@"GameDataDownloader" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"Could not set up any depots"}]);
            }); return;
        }

        __block int64_t totalFiles = 0;
        NSMutableArray *allFiles = [NSMutableArray array];
        for (NSDictionary *ds in depotSetups) {
            int depotId = [ds[@"depotId"] intValue];
            int64_t mid = (int64_t)[HARDCODED_MANIFEST_IDS()[@(depotId)] unsignedLongLongValue];
            uint64_t rc = [ds[@"requestCode"] unsignedLongLongValue];
            if (onProgress) onProgress([NSString stringWithFormat:@"Downloading manifest for depot %d...", depotId], 0);
            logToFile(@"[manifest] Downloading manifest for depot %d manifestId=%lld rc=%llu", depotId, (long long)mid, (unsigned long long)rc);
            NSData *manifestBytes = downloadManifestFromCDN(depotId, mid, rc, cdnHosts);
            if (!manifestBytes) { logToFile(@"[manifest] FAILED: downloadManifestFromCDN returned nil"); continue; }
            logToFile(@"[manifest] Got manifest: %lu bytes", (unsigned long)manifestBytes.length);
            NSArray *files = parseManifestFiles(manifestBytes, ds[@"depotKey"]);
            logToFile(@"[manifest] parseManifestFiles returned %lu files", (unsigned long)files.count);
            for (NSDictionary *f in files) {
                [allFiles addObject:@{@"depotId":@(depotId), @"file":f, @"depotKey":ds[@"depotKey"]}];
            }
            totalFiles += files.count;
        }

        if (allFiles.count == 0) {
            logToFile(@"[download] No files found in manifest(s)");
            dispatch_async(dispatch_get_main_queue(), ^{
                completion([NSError errorWithDomain:@"GameDataDownloader" code:-1 userInfo:@{NSLocalizedDescriptionKey:@"No files found"}]);
            }); return;
        }

        [[NSFileManager defaultManager] createDirectoryAtPath:targetDir withIntermediateDirectories:YES attributes:nil error:nil];

        __block int64_t completed = 0;
        __block BOOL anyFailed = NO;
        dispatch_semaphore_t sem = dispatch_semaphore_create(10);
        dispatch_group_t grp = dispatch_group_create();

        for (NSDictionary *entry in allFiles) {
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            dispatch_group_enter(grp);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                int depotId = [entry[@"depotId"] intValue];
                NSDictionary *mf = entry[@"file"];
                NSData *depotKey = entry[@"depotKey"];
                NSString *relPath = mf[@"path"];
                if ([relPath hasPrefix:[NSString stringWithFormat:@"%@/", gameDir]])
                    relPath = [relPath substringFromIndex:gameDir.length + 1];
                NSString *outPath = [targetDir stringByAppendingPathComponent:relPath];
                if (!assembleFile(depotId, mf, outPath, depotKey, cdnHosts)) anyFailed = YES;
                @synchronized (self) { completed++;
                    if (onProgress) dispatch_async(dispatch_get_main_queue(), ^{
                        onProgress([NSString stringWithFormat:@"Downloading %@... (%lld/%lld)",displayName,completed,totalFiles], (float)completed/(float)totalFiles);
                    });
                }
                dispatch_semaphore_signal(sem); dispatch_group_leave(grp);
            });
        }
        dispatch_group_wait(grp, DISPATCH_TIME_FOREVER);

        dispatch_async(dispatch_get_main_queue(), ^{
            if (anyFailed)
                completion([NSError errorWithDomain:@"GameDataDownloader" code:-1 userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"%@ downloaded with some errors",displayName]}]);
            else
                completion(nil);
        });
    });
}

@end
