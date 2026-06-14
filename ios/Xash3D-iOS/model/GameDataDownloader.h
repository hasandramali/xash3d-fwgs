#import <Foundation/Foundation.h>

@interface GameDataDownloader : NSObject

- (instancetype)initWithDocumentsDir:(NSString *)docsDir;

- (void)downloadGame:(NSString *)appId
          onProgress:(void(^)(NSString *status, float progress))onProgress
           completion:(void(^)(NSError *error))completion;

@end
