#import <Foundation/Foundation.h>

@interface GameDataDownloader : NSObject

- (instancetype)initWithDocumentsDir:(NSString *)docsDir;

- (BOOL)hasGameData:(NSString *)gameDir;
- (void)downloadGame:(NSString *)gameDir
          onProgress:(void(^)(NSString *status, float progress))onProgress
           completion:(void(^)(NSError *error))completion;

@end
