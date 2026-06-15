#import "GameLibDownloader.h"
#import <CommonCrypto/CommonCrypto.h>
#include "miniz.h"

static NSString *kManifestURL = @"https://github.com/FWGS/hlsdk-mega-build/releases/download/continuous/manifest.json";
static NSString *kReleaseBase = @"https://github.com/FWGS/hlsdk-mega-build/releases/download/continuous";
static const int kManifestVersion = 1;

static NSString *kPrefsSuite = @"com.xash3d.gamelib";
static NSString *kKeySHA = @"sha256";
static NSString *kKeyFilename = @"filename";
static NSString *kKeySource = @"source";
static NSString *kKeyDownloadTime = @"download_time";

@implementation GameLibEntry
@end

@interface GameLibDownloader ()
@property (nonatomic, strong) NSString *docsDir;
@property (nonatomic, strong) NSUserDefaults *prefs;
@end

@implementation GameLibDownloader

- (instancetype)initWithDocumentsDir:(NSString *)docsDir
{
    self = [super init];
    if (self) {
        _docsDir = [docsDir copy];
        _prefs = [[NSUserDefaults alloc] initWithSuiteName:kPrefsSuite];
    }
    return self;
}

- (NSString *)getArch
{
#if TARGET_OS_SIMULATOR
    return @"x86_64";
#else
    return @"arm64";
#endif
}

- (NSString *)libsBaseDir
{
    return [self.docsDir stringByAppendingPathComponent:@"gamelibs"];
}

- (BOOL)isDownloaded:(NSString *)gamedir
{
    NSString *dir = [[self libsBaseDir] stringByAppendingPathComponent:gamedir];
    BOOL isDir = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:dir isDirectory:&isDir] && isDir) {
        NSArray *contents = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
        if (contents.count > 0) return YES;
    }
    // case-insensitive fallback
    NSString *base = [self libsBaseDir];
    NSArray *all = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:base error:nil];
    for (NSString *name in all) {
        NSString *full = [base stringByAppendingPathComponent:name];
        BOOL subDir = NO;
        [[NSFileManager defaultManager] fileExistsAtPath:full isDirectory:&subDir];
        if (subDir && [name caseInsensitiveCompare:gamedir] == NSOrderedSame) {
            NSArray *sub = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:full error:nil];
            if (sub.count > 0) return YES;
        }
    }
    return NO;
}

#pragma mark - Manifest

- (NSString *)cacheFilePath
{
    NSString *caches = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES) firstObject];
    return [caches stringByAppendingPathComponent:@"hlsdk-manifest.json"];
}

- (void)fetchManifestWithCompletion:(void(^)(NSDictionary *manifest, NSError *error))completion
{
    NSURL *url = [NSURL URLWithString:kManifestURL];
    NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (error) {
            NSDictionary *cached = [self readCachedManifest];
            if (cached) { completion(cached, nil); return; }
            completion(nil, error);
            return;
        }
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        if (http.statusCode != 200) {
            NSDictionary *cached = [self readCachedManifest];
            if (cached) { completion(cached, nil); return; }
            completion(nil, [NSError errorWithDomain:@"GameLibDownloader" code:http.statusCode userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"HTTP %ld", (long)http.statusCode]}]);
            return;
        }
        NSError *jsonErr = nil;
        NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jsonErr];
        if (!json || jsonErr) {
            NSDictionary *cached = [self readCachedManifest];
            if (cached) { completion(cached, nil); return; }
            completion(nil, jsonErr);
            return;
        }
        NSString *versionErr = [self checkManifestVersion:json];
        if (versionErr) {
            NSDictionary *cached = [self readCachedManifest];
            if (cached) { completion(cached, nil); return; }
            completion(nil, [NSError errorWithDomain:@"GameLibDownloader" code:0 userInfo:@{NSLocalizedDescriptionKey: versionErr}]);
            return;
        }
        [data writeToFile:[self cacheFilePath] atomically:YES];
        completion(json, nil);
    }];
    [task resume];
}

- (NSDictionary *)readCachedManifest
{
    NSString *path = [self cacheFilePath];
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return nil;
    NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (!json || [self checkManifestVersion:json]) return nil;
    return json;
}

- (NSString *)checkManifestVersion:(NSDictionary *)json
{
    id ver = json[@"version"];
    if (!ver) return @"Manifest missing 'version' field";
    if (![ver isKindOfClass:[NSNumber class]] || [ver intValue] != kManifestVersion)
        return [NSString stringWithFormat:@"Unsupported manifest version: %@ (expected %d)", ver, kManifestVersion];
    return nil;
}

- (GameLibEntry *)lookupEntry:(NSDictionary *)manifest gamedir:(NSString *)gamedir
{
    NSString *arch = [self getArch];
    if (!arch) return nil;
    NSString *platformArch = [NSString stringWithFormat:@"apple-%@", arch];

    NSDictionary *mods = manifest[@"mods"];
    if (![mods isKindOfClass:[NSDictionary class]]) return nil;

    NSString *modKey = nil;
    for (NSString *key in mods) {
        if ([key caseInsensitiveCompare:gamedir] == NSOrderedSame) {
            modKey = key;
            break;
        }
    }
    if (!modKey) return nil;

    NSDictionary *builds = mods[modKey][@"builds"];
    if (![builds isKindOfClass:[NSDictionary class]]) return nil;
    NSDictionary *build = builds[platformArch];
    if (!build) return nil;

    NSString *filename = build[@"filename"] ?: @"";
    NSString *sha256 = build[@"sha256"] ?: @"";
    if (filename.length == 0 || sha256.length == 0) return nil;

    GameLibEntry *entry = [[GameLibEntry alloc] init];
    entry.modKey = modKey;
    entry.platformArch = platformArch;
    entry.filename = filename;
    entry.sha256 = sha256.lowercaseString;
    id sourceObj = build[@"source"];
    entry.sourceJson = sourceObj ? [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:sourceObj options:0 error:nil] encoding:NSUTF8StringEncoding] : nil;
    return entry;
}

- (void)lookupBuild:(NSString *)gamedir completion:(void(^)(GameLibEntry *entry, NSString *error))completion
{
    [self fetchManifestWithCompletion:^(NSDictionary *manifest, NSError *error) {
        if (error) { completion(nil, error.localizedDescription); return; }
        GameLibEntry *entry = [self lookupEntry:manifest gamedir:gamedir];
        if (!entry) { completion(nil, [NSString stringWithFormat:@"No build for '%@' on apple-%@", gamedir, [self getArch]]); return; }
        completion(entry, nil);
    }];
}

- (void)isUpdateAvailable:(NSString *)gamedir completion:(void(^)(BOOL available, NSError *error))completion
{
    [self fetchManifestWithCompletion:^(NSDictionary *manifest, NSError *error) {
        if (error) { completion(NO, error); return; }
        GameLibEntry *entry = [self lookupEntry:manifest gamedir:gamedir];
        if (!entry) { completion(NO, nil); return; }
        NSString *stored = [self.prefs stringForKey:[NSString stringWithFormat:@"%@_%@", gamedir, kKeySHA]];
        if (!stored) { completion(YES, nil); return; }
        completion(![stored.lowercaseString isEqualToString:entry.sha256.lowercaseString], nil);
    }];
}

#pragma mark - Download

- (void)download:(NSString *)gamedir onProgress:(void(^)(float))onProgress completion:(void(^)(NSError *))completion
{
    [self fetchManifestWithCompletion:^(NSDictionary *manifest, NSError *error) {
        if (error) { completion(error); return; }
        GameLibEntry *entry = [self lookupEntry:manifest gamedir:gamedir];
        if (!entry) {
            completion([NSError errorWithDomain:@"GameLibDownloader" code:0 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"No build for '%@' on apple-%@", gamedir, [self getArch]]}]);
            return;
        }

        NSString *urlStr = [NSString stringWithFormat:@"%@/%@", kReleaseBase, entry.filename];
        NSURL *url = [NSURL URLWithString:urlStr];
        NSString *tempPath = [NSTemporaryDirectory() stringByAppendingPathComponent:entry.filename];

        // Download
        NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration defaultSessionConfiguration];
        NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg delegate:nil delegateQueue:[NSOperationQueue mainQueue]];
        __weak typeof(self) weakSelf = self;
        NSURLSessionDownloadTask *task = [session downloadTaskWithURL:url completionHandler:^(NSURL *location, NSURLResponse *response, NSError *err) {
            if (err) { completion(err); return; }
            NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
            if (http.statusCode != 200) {
                completion([NSError errorWithDomain:@"GameLibDownloader" code:http.statusCode userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"HTTP %ld", (long)http.statusCode]}]);
                return;
            }
            // Move temp file
            [[NSFileManager defaultManager] removeItemAtPath:tempPath error:nil];
            [[NSFileManager defaultManager] moveItemAtURL:location toURL:[NSURL fileURLWithPath:tempPath] error:&err];
            if (err) { completion(err); return; }

            // Verify SHA-256
            NSString *actualSha = [weakSelf computeSHA256:tempPath];
            if (![actualSha.lowercaseString isEqualToString:entry.sha256.lowercaseString]) {
                [[NSFileManager defaultManager] removeItemAtPath:tempPath error:nil];
                completion([NSError errorWithDomain:@"GameLibDownloader" code:0 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"SHA-256 mismatch: expected %@, got %@", entry.sha256, actualSha]}]);
                return;
            }

            // Extract zip
            NSString *destDir = [weakSelf libsBaseDir];
            [[NSFileManager defaultManager] createDirectoryAtPath:destDir withIntermediateDirectories:YES attributes:nil error:nil];

            // Remove prior install
            NSArray *existing = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:destDir error:nil];
            for (NSString *name in existing) {
                NSString *full = [destDir stringByAppendingPathComponent:name];
                BOOL isDir = NO;
                [[NSFileManager defaultManager] fileExistsAtPath:full isDirectory:&isDir];
                if (isDir && [name caseInsensitiveCompare:entry.modKey] == NSOrderedSame)
                    [[NSFileManager defaultManager] removeItemAtPath:full error:nil];
            }

            NSError *extractErr = [weakSelf extractZip:tempPath toDir:destDir];
            if (extractErr) {
                [[NSFileManager defaultManager] removeItemAtPath:tempPath error:nil];
                [[NSFileManager defaultManager] removeItemAtPath:[destDir stringByAppendingPathComponent:entry.modKey] error:nil];
                completion(extractErr);
                return;
            }

            // Store metadata
            [weakSelf.prefs setObject:entry.sha256 forKey:[NSString stringWithFormat:@"%@_%@", gamedir, kKeySHA]];
            [weakSelf.prefs setObject:entry.filename forKey:[NSString stringWithFormat:@"%@_%@", gamedir, kKeyFilename]];
            if (entry.sourceJson) [weakSelf.prefs setObject:entry.sourceJson forKey:[NSString stringWithFormat:@"%@_%@", gamedir, kKeySource]];
            [weakSelf.prefs setObject:@([[NSDate date] timeIntervalSince1970]) forKey:[NSString stringWithFormat:@"%@_%@", gamedir, kKeyDownloadTime]];

            [[NSFileManager defaultManager] removeItemAtPath:tempPath error:nil];
            completion(nil);
        }];
        [task resume];
    }];
}

#pragma mark - Zip extraction

- (NSError *)extractZip:(NSString *)zipPath toDir:(NSString *)destDir
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, [zipPath UTF8String], 0)) {
        return [NSError errorWithDomain:@"GameLibDownloader" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Failed to open zip archive"}];
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    NSString *destCanon = [[NSURL fileURLWithPath:destDir].URLByStandardizingPath path];
    NSError *err = nil;

    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        NSString *name = [NSString stringWithUTF8String:stat.m_filename];
        if (!name) continue;

        NSString *fullPath = [destDir stringByAppendingPathComponent:name];
        NSString *fullCanon = [[NSURL fileURLWithPath:fullPath].URLByStandardizingPath path];
        if (![fullCanon hasPrefix:destCanon]) {
            err = [NSError errorWithDomain:@"GameLibDownloader" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Zip path traversal detected"}];
            break;
        }

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            [[NSFileManager defaultManager] createDirectoryAtPath:fullPath withIntermediateDirectories:YES attributes:nil error:nil];
        } else {
            [[NSFileManager defaultManager] createDirectoryAtPath:[fullPath stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
            size_t outSize = 0;
            void *data = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
            if (data) {
                NSData *nsdata = [NSData dataWithBytesNoCopy:data length:outSize freeWhenDone:YES];
                [nsdata writeToFile:fullPath atomically:YES];
            }
        }
    }

    mz_zip_reader_end(&zip);
    return err;
}

#pragma mark - SHA-256

- (NSString *)computeSHA256:(NSString *)filePath
{
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:filePath];
    if (!fh) return nil;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    NSData *buffer = nil;
    while ((buffer = [fh readDataOfLength:65536]).length > 0) {
        CC_SHA256_Update(&ctx, buffer.bytes, (CC_LONG)buffer.length);
    }
    [fh closeFile];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        [hex appendFormat:@"%02x", digest[i]];
    return hex;
}

#pragma mark - Source info

- (NSDictionary *)sourceInfoFor:(NSString *)gamedir
{
    NSString *raw = [self.prefs stringForKey:[NSString stringWithFormat:@"%@_%@", gamedir, kKeySource]];
    if (!raw) return nil;
    return [NSJSONSerialization JSONObjectWithData:[raw dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
}

- (NSTimeInterval)downloadTimeFor:(NSString *)gamedir
{
    return [self.prefs doubleForKey:[NSString stringWithFormat:@"%@_%@", gamedir, kKeyDownloadTime]];
}

+ (NSString *)manifestURL { return kManifestURL; }
+ (NSString *)releaseBaseURL { return kReleaseBase; }

@end
