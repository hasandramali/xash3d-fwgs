#import <Foundation/Foundation.h>

@interface GameLibEntry : NSObject
@property (nonatomic, strong) NSString *modKey;
@property (nonatomic, strong) NSString *platformArch;
@property (nonatomic, strong) NSString *filename;
@property (nonatomic, strong) NSString *sha256;
@property (nonatomic, strong) NSString *sourceJson;
@end

@interface GameLibDownloader : NSObject

- (instancetype)initWithDocumentsDir:(NSString *)docsDir;

- (NSString *)getArch;
- (NSString *)libsBaseDir;
- (BOOL)isDownloaded:(NSString *)gamedir;

- (void)fetchManifestWithCompletion:(void(^)(NSDictionary *manifest, NSError *error))completion;
- (void)lookupBuild:(NSString *)gamedir completion:(void(^)(GameLibEntry *entry, NSString *error))completion;
- (void)isUpdateAvailable:(NSString *)gamedir completion:(void(^)(BOOL available, NSError *error))completion;

- (void)download:(NSString *)gamedir
     onProgress:(void(^)(float progress))onProgress
      completion:(void(^)(NSError *error))completion;

+ (NSString *)manifestURL;
+ (NSString *)releaseBaseURL;

@end
