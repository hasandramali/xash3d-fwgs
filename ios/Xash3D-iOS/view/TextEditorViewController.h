#import <UIKit/UIKit.h>

@interface TextEditorViewController : UIViewController
@property (nonatomic, strong) NSString *filePath;
- (instancetype)initWithFilePath:(NSString *)path;
@end
