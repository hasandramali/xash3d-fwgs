/*
 launchdialog.m - iOS lauch dialog
 Copyright (C) 2016 mittorn
 Copyright (C) 2024 Xash3D FWGS Contributors
 GPLv3+
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <sys/stat.h>
#include "launcherdialog.h"

char *g_szLibrarySuffix = NULL;
float g_iOSVer;

extern int Q_buildnum( void );

XashGameStatus_t g_iStartGameStatus = XGS_SKIP;

int g_iArgc = 0;
char **g_pszArgv = NULL;

extern "C" int IOS_GetArgs( char ***out )
{
    *out = g_pszArgv;
    return g_iArgc;
}

extern "C" const char *IOS_GetExecDir()
{
    return IOS_GetBundleDir();
}

void IOS_StartBackgroundTask() { /* */ }

extern "C" const char *IOS_GetDocsDir()
{
    static const char *dir = nil;
    if( dir ) return dir;
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirctory = [paths objectAtIndex:0];
    [[NSFileManager defaultManager] createDirectoryAtPath:documentsDirctory withIntermediateDirectories:YES attributes:nil error:nil];
    dir = [documentsDirctory fileSystemRepresentation];
    return dir;
}

extern "C" const char *IOS_GetBundleDir()
{
    NSString *path = [[NSBundle mainBundle] bundlePath];
    static char c_path[256];
    strncpy(c_path, [path UTF8String], sizeof(c_path));
    c_path[sizeof(c_path) - 1] = '\0';
    return c_path;
}

extern "C" BOOL IOS_IsResourcesReady()
{
    NSString *doc = [NSString stringWithUTF8String:IOS_GetDocsDir()];
    NSFileManager *fileManager = [NSFileManager defaultManager];
    return ([fileManager fileExistsAtPath:[doc stringByAppendingPathComponent:@"valve"]] ||
            [fileManager fileExistsAtPath:[doc stringByAppendingPathComponent:@"cstrike"]]);
}

void IOS_PrepareView()
{
    UIWindow *window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *storyboardName = @"TutorStoryboard";
    UIStoryboard *storyboard = [UIStoryboard storyboardWithName:storyboardName bundle:bundle];
    UIViewController * controller = storyboard.instantiateInitialViewController;
    [window setRootViewController:controller];
    [window makeKeyAndVisible];
}

extern "C" void IOS_SetDefaultArgs()
{
    static char width_str[32] = "0";
    static char height_str[32] = "0";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wwritable-strings"
    static char *args[64] = { "xash", "-dev", "1", "-game", "valve", "-width", width_str, "-height", height_str };
#pragma clang diagnostic pop
    CGRect rect_screen = [[UIScreen mainScreen]bounds];
    CGSize size_screen = rect_screen.size;
    CGFloat scale_screen = [UIScreen mainScreen].scale;
    CGFloat width = size_screen.width * scale_screen;
    CGFloat height = size_screen.height * scale_screen;
    snprintf(width_str, sizeof(width_str), "%d", (int)width);
    snprintf(height_str, sizeof(height_str), "%d", (int)height);
    g_pszArgv = args;
    g_iArgc = 10;
}

extern "C" void IOS_LaunchDialog( void )
{
    NSString *ver = [[UIDevice currentDevice] systemVersion];
    g_iOSVer = [ver floatValue];

    if(!IOS_IsResourcesReady())
    {
        g_iStartGameStatus = XGS_WAITING;
        IOS_PrepareView();
    }
    @autoreleasepool {
        while( g_iStartGameStatus == XGS_WAITING ) {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantFuture]];
        }
    }
    IOS_SetDefaultArgs();
}

extern "C" char *IOS_GetUDID( void )
{
    static char udid[256];
    NSString *id = [[[UIDevice currentDevice]identifierForVendor] UUIDString];
    strncpy( udid, [id UTF8String], 255 );
    return udid;
}

extern "C" void IOS_Log(const char *text)
{
    NSString *nstext =[NSString stringWithUTF8String:text];
    NSLog(@"Xash: %@", nstext);
}

extern "C" void IOS_OpenURL(const char *url)
{
    [[UIApplication sharedApplication] openURL:[NSURL URLWithString:[[NSString alloc] initWithUTF8String:url]] options:@{} completionHandler:nil];
}

extern "C" void IOS_GetSystemVersion(int *major, int *minor, int *patch)
{
    auto ver = [[NSProcessInfo processInfo] operatingSystemVersion];
    if(major) *major = (int)ver.majorVersion;
    if(minor) *minor = (int)ver.minorVersion;
    if(patch) *patch = (int)ver.patchVersion;
}
