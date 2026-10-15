#import <Foundation/Foundation.h>

@interface GameDataDownloader : NSObject
@property (atomic, readonly) BOOL installing;
- (void)cancel;

- (instancetype)initWithDocumentsDir:(NSString *)docsDir;

- (BOOL)hasGameData:(NSString *)gameDir;
- (void)downloadGame:(NSString *)gameDir
          onProgress:(void(^)(NSString *status, float progress))onProgress
           completion:(void(^)(NSError *error))completion;

@end
