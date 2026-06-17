#import <UIKit/UIKit.h>
@class GameDataDownloader;

@interface FileBrowserViewController : UITableViewController
@property (nonatomic, strong) NSString *currentPath;
@property (nonatomic, strong) GameDataDownloader *dataDownloader;
- (instancetype)initWithPath:(NSString *)path;
@end
