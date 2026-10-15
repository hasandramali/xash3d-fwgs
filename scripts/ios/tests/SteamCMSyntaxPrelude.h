#include <atomic>
#include <mutex>
#include <vector>
#include <unistd.h>

#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#else
typedef signed char BOOL;
typedef unsigned long NSUInteger;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil nullptr
@class NSString, NSData;
@interface NSObject
+ (instancetype)alloc;
@end
@interface NSString : NSObject
+ (instancetype)stringWithFormat:(NSString *)format, ...;
@end
@interface NSDictionary : NSObject
+ (instancetype)dictionaryWithObjects:(const id[])objects forKeys:(const id[])keys count:(NSUInteger)count;
- (id)objectForKeyedSubscript:(id)key;
@end
@interface NSNumber : NSObject
- (int)intValue;
@end
typedef struct {
    unsigned long state;
    id __unsafe_unretained *itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long extra[5];
} NSFastEnumerationState;
@interface NSArray<ObjectType> : NSObject
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                objects:(id __unsafe_unretained [])objects count:(NSUInteger)count;
@end
@interface NSData : NSObject
@property(readonly) const void *bytes;
@property(readonly) NSUInteger length;
+ (instancetype)dataWithBytes:(const void *)bytes length:(NSUInteger)length;
@end
@interface NSError : NSObject
+ (instancetype)errorWithDomain:(NSString *)domain code:(long)code userInfo:(NSDictionary *)userInfo;
@end
@interface NSThread : NSObject
@property(readonly) BOOL isCancelled;
@property(copy) NSString *name;
+ (NSThread *)currentThread;
+ (void)sleepForTimeInterval:(double)interval;
- (instancetype)initWithBlock:(void (^)(void))block;
- (void)start;
- (void)cancel;
@end
extern NSString *NSLocalizedDescriptionKey;
#endif

void logToFile(NSString *format, ...);
