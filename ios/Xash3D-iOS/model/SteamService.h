#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSNotificationName const SvenSteamStateChanged;

@interface SteamService : NSObject
@property (atomic, readonly) BOOL loggedIn;
@property (atomic, readonly) BOOL busy;
@property (atomic, readonly) BOOL brokerRunning;
@property (atomic, readonly) BOOL hasSavedSession;
@property (atomic, copy, readonly) NSString *accountName;
@property (atomic, copy, readonly) NSString *status;
@property (atomic, copy, readonly) NSString *guardKind;
@property (atomic) BOOL autoConnect;
+ (instancetype)shared;
- (void)login:(NSString *)username password:(NSString *)password;
- (void)submitCode:(NSString *)code;
- (void)restoreSession;
- (void)cancelLogin;
- (void)logout;
- (void)startBroker;
- (void)stopBroker;
@end
