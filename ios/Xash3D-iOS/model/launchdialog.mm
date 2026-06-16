/*
 launchdialog.m - iOS lauch dialog
 Copyright (C) 2016 mittorn
 Copyright (C) 2024 Xash3D FWGS Contributors
 GPLv3+
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include "launcherdialog.h"
#include "FileBrowserViewController.h"

char *g_szLibrarySuffix = NULL;
float g_iOSVer;
static UIWindow *g_launcherWindow = nil;
static FILE *g_iosLogFile = NULL;

extern int Q_buildnum( void );

extern "C" XashGameStatus_t g_iStartGameStatus = XGS_SKIP;
extern "C" int g_iArgc = 0;
extern "C" char **g_pszArgv = NULL;

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

static void IOS_WriteLogLine( const char *text )
{
    if( !text ) text = "";
    if( !g_iosLogFile )
    {
        char path[1024];
        snprintf( path, sizeof( path ), "%s/xash_ios.log", IOS_GetDocsDir() );
        g_iosLogFile = fopen( path, "a" );
    }

    if( g_iosLogFile )
    {
        fprintf( g_iosLogFile, "%s\n", text );
        fflush( g_iosLogFile );
    }
}

static void IOS_SignalHandler( int sig )
{
    char msg[64];
    int len = snprintf( msg, sizeof( msg ), "Xash: fatal signal %d\n", sig );
    if( len > 0 )
    {
        char path[1024];
        snprintf( path, sizeof( path ), "%s/xash_ios.log", IOS_GetDocsDir() );
        int fd = open( path, O_WRONLY | O_CREAT | O_APPEND, 0644 );
        if( fd >= 0 )
        {
            write( fd, msg, (size_t)len );
            close( fd );
        }
    }
    signal( sig, SIG_DFL );
    raise( sig );
}

static void IOS_UncaughtExceptionHandler( NSException *exception )
{
    NSString *line = [NSString stringWithFormat:@"Xash: uncaught exception %@: %@\n%@",
                      exception.name, exception.reason, exception.callStackSymbols];
    IOS_WriteLogLine( [line UTF8String] );
}

static void IOS_InstallLogHandlers()
{
    static BOOL installed = NO;
    if( installed ) return;
    installed = YES;

    NSSetUncaughtExceptionHandler( IOS_UncaughtExceptionHandler );
    signal( SIGABRT, IOS_SignalHandler );
    signal( SIGBUS, IOS_SignalHandler );
    signal( SIGFPE, IOS_SignalHandler );
    signal( SIGILL, IOS_SignalHandler );
    signal( SIGSEGV, IOS_SignalHandler );

    IOS_WriteLogLine( "Xash: iOS launcher starting" );
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
    NSString *docs = [NSString stringWithUTF8String:IOS_GetDocsDir()];
    FileBrowserViewController *fbc = [[FileBrowserViewController alloc] initWithPath:docs];
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:fbc];
    g_launcherWindow = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    [g_launcherWindow setRootViewController:nav];
    [g_launcherWindow makeKeyAndVisible];
    IOS_WriteLogLine( "Xash: launcher view prepared" );
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
    IOS_InstallLogHandlers();

    NSString *ver = [[UIDevice currentDevice] systemVersion];
    g_iOSVer = [ver floatValue];

    g_iStartGameStatus = XGS_WAITING;
    IOS_PrepareView();

    @autoreleasepool {
        while( g_iStartGameStatus == XGS_WAITING ) {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantFuture]];
        }
    }

    IOS_WriteLogLine( "Xash: launcher finished, starting engine" );
    [g_launcherWindow setHidden:YES];
    [g_launcherWindow setRootViewController:nil];
    g_launcherWindow = nil;

    // Force landscape orientation before engine starts
    if (@available(iOS 16.0, *)) {
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if ([scene isKindOfClass:[UIWindowScene class]]) {
                UIWindowScene *ws = (UIWindowScene *)scene;
                UIWindowSceneGeometryPreferencesIOS *prefs = [[UIWindowSceneGeometryPreferencesIOS alloc] init];
                prefs.interfaceOrientations = UIInterfaceOrientationMaskLandscape;
                [ws requestGeometryUpdateWithPreferences:prefs errorHandler:nil];
            }
        }
    } else {
        [[UIDevice currentDevice] setValue:@(UIInterfaceOrientationLandscapeLeft) forKey:@"orientation"];
    }
    // Small delay for rotation to take effect
    usleep(500000);

    if( !g_pszArgv )
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
    IOS_WriteLogLine( text );
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
