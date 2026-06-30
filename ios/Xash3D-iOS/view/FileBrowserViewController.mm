#import "FileBrowserViewController.h"
#import "TextEditorViewController.h"
#import "GameDataDownloader.h"
#include "launcherdialog.h"
#import <objc/runtime.h>

@interface _BlockAction : NSObject
@property (nonatomic, copy) void (^block)(void);
- (void)handleTap;
@end
@implementation _BlockAction
- (void)handleTap { if (self.block) self.block(); }
@end

@interface FileBrowserViewController () <UIDocumentPickerDelegate, UIAlertViewDelegate>
@property (nonatomic, strong) NSArray *directories;
@property (nonatomic, strong) NSArray *files;
@property (nonatomic, strong) NSFileManager *fm;
@property (nonatomic, assign) BOOL forceLandscape;
@end

static NSString *_pasteboardPath = nil;
static BOOL _isMoveOperation = NO;

static NSString *kCellID = @"FileCell";

@implementation FileBrowserViewController

- (instancetype)initWithPath:(NSString *)path
{
    self = [super initWithStyle:UITableViewStylePlain];
    if (self) {
        _currentPath = [path copy];
        _fm = [NSFileManager defaultManager];
        NSString *docsDir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
        _dataDownloader = [[GameDataDownloader alloc] initWithDocumentsDir:docsDir];
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = [self.currentPath lastPathComponent];
    if (self.title.length == 0 || [self.currentPath isEqualToString:NSHomeDirectory()])
        self.title = @"Xash3D";

    // Back button (left) — only visible when not at root
    NSString *docs = [NSString stringWithUTF8String:IOS_GetDocsDir()];
    if (![self.currentPath isEqualToString:docs]) {
        UIBarButtonItem *backBtn = [[UIBarButtonItem alloc] initWithTitle:@"Back" style:UIBarButtonItemStylePlain target:self action:@selector(backTapped)];
        self.navigationItem.leftBarButtonItem = backBtn;
    }

    // Launch button (right)
    UIBarButtonItem *launchBtn = [[UIBarButtonItem alloc] initWithTitle:@"Launch" style:UIBarButtonItemStyleDone target:self action:@selector(launchTapped)];
    self.navigationItem.rightBarButtonItem = launchBtn;

    UIBarButtonItem *importBtn = [[UIBarButtonItem alloc] initWithTitle:@"Import" style:UIBarButtonItemStylePlain target:self action:@selector(importTapped)];
    UIBarButtonItem *flex = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil];
    UIBarButtonItem *folderBtn = [[UIBarButtonItem alloc] initWithTitle:@"New Folder" style:UIBarButtonItemStylePlain target:self action:@selector(newFolderTapped)];
    UIBarButtonItem *downloadDataBtn = [[UIBarButtonItem alloc] initWithTitle:@"Download Data" style:UIBarButtonItemStylePlain target:self action:@selector(downloadDataTapped)];
    UIBarButtonItem *pasteBtn = [[UIBarButtonItem alloc] initWithTitle:@"Paste" style:UIBarButtonItemStylePlain target:self action:@selector(pasteTapped)];
    pasteBtn.enabled = (_pasteboardPath != nil);
    self.toolbarItems = @[importBtn, flex, folderBtn, flex, downloadDataBtn, flex, pasteBtn];
    self.navigationController.toolbarHidden = NO;

    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:kCellID];
    [self reloadFiles];
}

- (void)backTapped
{
    [self.navigationController popViewControllerAnimated:YES];
}

- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];
    self.navigationController.toolbarHidden = NO;
    [self reloadFiles];
    [self updatePasteButton];
}

- (NSArray *)availableGameDirs
{
    NSString *docs = [NSString stringWithUTF8String:IOS_GetDocsDir()];
    NSMutableArray *dirs = [NSMutableArray array];
    NSArray *contents = [self.fm contentsOfDirectoryAtPath:docs error:nil];
    for (NSString *name in contents) {
        if ([name hasPrefix:@"."]) continue;
        NSString *full = [docs stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        if ([self.fm fileExistsAtPath:full isDirectory:&isDir] && isDir) {
            NSString *liblist = [full stringByAppendingPathComponent:@"liblist.gam"];
            if ([self.fm fileExistsAtPath:liblist]) {
                [dirs addObject:name];
            }
        }
    }
    // Also include known dirs even if no liblist.gam found yet
    NSArray *known = @[@"valve", @"cstrike", @"gearbox", @"tfc", @"czero", @"dod"];
    for (NSString *name in known) {
        if (![dirs containsObject:name]) {
            NSString *full = [docs stringByAppendingPathComponent:name];
            BOOL isDir = NO;
            if ([self.fm fileExistsAtPath:full isDirectory:&isDir] && isDir) {
                [dirs addObject:name];
            }
        }
    }
    return [dirs sortedArrayUsingSelector:@selector(compare:)];
}

- (void)launchTapped
{
    NSArray *gameDirs = [self availableGameDirs];
    if (gameDirs.count == 0) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"No Games Found" message:@"Download game data or transfer game folders to Documents directory." preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }
    UIAlertController *picker = [UIAlertController alertControllerWithTitle:@"Select Game" message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSString *name in gameDirs) {
        [picker addAction:[UIAlertAction actionWithTitle:name style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
            [self showLaunchDialogForGame:name];
        }]];
    }
    [picker addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad)
        picker.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItem;
    [self presentViewController:picker animated:YES completion:nil];
}

- (NSString *)botDllForGame:(NSString *)gamedir
{
    if ([gamedir isEqualToString:@"cstrike"])
        return @"dlls/yapb_arm64.dylib";
    if ([gamedir isEqualToString:@"valve"])
        return @"dlls/bot_arm64.dylib";
    return nil;
}

- (void)showLaunchDialogForGame:(NSString *)gamedir
{
    NSString *savedArgs = [[NSUserDefaults standardUserDefaults] stringForKey:[NSString stringWithFormat:@"launch_args_%@", gamedir]];
    NSString *args = savedArgs ?: @"-dev 1 -console -log";
    NSString *botDll = [self botDllForGame:gamedir];
    [self showArgsDialogForGame:gamedir args:args botDll:botDll];
}

- (void)showArgsDialogForGame:(NSString *)gamedir args:(NSString *)defaultArgs botDll:(NSString *)botDll
{
    UIViewController *vc = [[UIViewController alloc] init];
    vc.modalPresentationStyle = UIModalPresentationPageSheet;
    vc.preferredContentSize = CGSizeMake(320, 340);

    UIView *root = vc.view;
    root.backgroundColor = [UIColor systemBackgroundColor];

    // Scroll view for keyboard avoidance
    UIScrollView *scrollView = [[UIScrollView alloc] init];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [root addSubview:scrollView];

    // Content view inside scroll view
    UIView *contentView = [[UIView alloc] init];
    contentView.translatesAutoresizingMaskIntoConstraints = NO;
    contentView.backgroundColor = [UIColor clearColor];
    [scrollView addSubview:contentView];

    // Title
    UILabel *titleLbl = [[UILabel alloc] init];
    titleLbl.text = [NSString stringWithFormat:@"Launch %@", gamedir];
    titleLbl.font = [UIFont boldSystemFontOfSize:17];
    titleLbl.textAlignment = NSTextAlignmentCenter;
    titleLbl.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:titleLbl];

    // Immersive Mode label
    UILabel *imLbl = [[UILabel alloc] init];
    imLbl.text = @"Immersive Mode";
    imLbl.font = [UIFont systemFontOfSize:15];
    imLbl.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:imLbl];

    // Immersive Mode switch
    UISwitch *imSwitch = [[UISwitch alloc] init];
    imSwitch.on = NO;
    NSString *imKey = [NSString stringWithFormat:@"immersive_mode_%@", gamedir];
    id saved = [[NSUserDefaults standardUserDefaults] objectForKey:imKey];
    if (saved) imSwitch.on = [saved boolValue];
    imSwitch.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:imSwitch];

    // Enable Bots switch (only if botDll available)
    UISwitch *botSwitch = nil;
    UILabel *botLbl = nil;
    NSString *botsKey = [NSString stringWithFormat:@"bots_enabled_%@", gamedir];
    if (botDll.length > 0) {
        botLbl = [[UILabel alloc] init];
        botLbl.text = @"Enable Bots";
        botLbl.font = [UIFont systemFontOfSize:15];
        botLbl.translatesAutoresizingMaskIntoConstraints = NO;
        [contentView addSubview:botLbl];

        botSwitch = [[UISwitch alloc] init];
        botSwitch.on = [[NSUserDefaults standardUserDefaults] boolForKey:botsKey];
        botSwitch.translatesAutoresizingMaskIntoConstraints = NO;
        [contentView addSubview:botSwitch];
    }

    // Args text field
    UITextField *argField = [[UITextField alloc] init];
    argField.text = defaultArgs;
    argField.placeholder = @"Extra arguments";
    argField.borderStyle = UITextBorderStyleRoundedRect;
    argField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    argField.font = [UIFont systemFontOfSize:14];
    argField.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:argField];

    // Cancel button
    UIButton *cancelBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [cancelBtn setTitle:@"Cancel" forState:UIControlStateNormal];
    cancelBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:cancelBtn];

    // Launch button
    UIButton *launchBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [launchBtn setTitle:@"Launch" forState:UIControlStateNormal];
    launchBtn.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    launchBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:launchBtn];

    // Separator line
    UIView *sep = [[UIView alloc] init];
    sep.backgroundColor = [UIColor separatorColor];
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:sep];

    // Vertical divider between buttons
    UIView *div = [[UIView alloc] init];
    div.backgroundColor = [UIColor separatorColor];
    div.translatesAutoresizingMaskIntoConstraints = NO;
    [contentView addSubview:div];

    // Scroll view fills root
    [NSLayoutConstraint activateConstraints:@[
        [scrollView.topAnchor constraintEqualToAnchor:root.topAnchor],
        [scrollView.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
    ]];

    // Content view fills scroll view width, determines height
    [NSLayoutConstraint activateConstraints:@[
        [contentView.topAnchor constraintEqualToAnchor:scrollView.topAnchor],
        [contentView.leadingAnchor constraintEqualToAnchor:scrollView.leadingAnchor],
        [contentView.trailingAnchor constraintEqualToAnchor:scrollView.trailingAnchor],
        [contentView.bottomAnchor constraintEqualToAnchor:scrollView.bottomAnchor],
        [contentView.widthAnchor constraintEqualToAnchor:scrollView.widthAnchor],
    ]];

    // Accumulator for vertical chain
    UIView *prevView;

    // Title at top of content view
    [NSLayoutConstraint activateConstraints:@[
        [titleLbl.topAnchor constraintEqualToAnchor:contentView.topAnchor constant:16],
        [titleLbl.centerXAnchor constraintEqualToAnchor:contentView.centerXAnchor],
        [titleLbl.leadingAnchor constraintGreaterThanOrEqualToAnchor:contentView.leadingAnchor constant:16],
        [titleLbl.trailingAnchor constraintLessThanOrEqualToAnchor:contentView.trailingAnchor constant:-16],
    ]];
    prevView = titleLbl;

    // Immersive Mode row
    [NSLayoutConstraint activateConstraints:@[
        [imLbl.topAnchor constraintEqualToAnchor:prevView.bottomAnchor constant:24],
        [imLbl.leadingAnchor constraintEqualToAnchor:contentView.leadingAnchor constant:20],
        [imLbl.centerYAnchor constraintEqualToAnchor:imSwitch.centerYAnchor],
        [imSwitch.topAnchor constraintEqualToAnchor:imLbl.topAnchor],
        [imSwitch.trailingAnchor constraintEqualToAnchor:contentView.trailingAnchor constant:-20],
    ]];
    prevView = imSwitch;

    // Bots row (if applicable)
    if (botDll.length > 0 && botLbl && botSwitch) {
        [NSLayoutConstraint activateConstraints:@[
            [botLbl.topAnchor constraintEqualToAnchor:prevView.bottomAnchor constant:16],
            [botLbl.leadingAnchor constraintEqualToAnchor:contentView.leadingAnchor constant:20],
            [botLbl.centerYAnchor constraintEqualToAnchor:botSwitch.centerYAnchor],
            [botSwitch.topAnchor constraintEqualToAnchor:botLbl.topAnchor],
            [botSwitch.trailingAnchor constraintEqualToAnchor:contentView.trailingAnchor constant:-20],
        ]];
        prevView = botSwitch;
    }

    // Args text field
    [NSLayoutConstraint activateConstraints:@[
        [argField.topAnchor constraintEqualToAnchor:prevView.bottomAnchor constant:16],
        [argField.leadingAnchor constraintEqualToAnchor:contentView.leadingAnchor constant:20],
        [argField.trailingAnchor constraintEqualToAnchor:contentView.trailingAnchor constant:-20],
        [argField.heightAnchor constraintEqualToConstant:34],
    ]];
    prevView = argField;

    // Separator
    [NSLayoutConstraint activateConstraints:@[
        [sep.topAnchor constraintEqualToAnchor:prevView.bottomAnchor constant:20],
        [sep.leadingAnchor constraintEqualToAnchor:contentView.leadingAnchor],
        [sep.trailingAnchor constraintEqualToAnchor:contentView.trailingAnchor],
        [sep.heightAnchor constraintEqualToConstant:1 / [UIScreen mainScreen].scale],
    ]];
    prevView = sep;

    // Bottom buttons row
    [NSLayoutConstraint activateConstraints:@[
        [cancelBtn.topAnchor constraintEqualToAnchor:prevView.bottomAnchor],
        [cancelBtn.leadingAnchor constraintEqualToAnchor:contentView.leadingAnchor],
        [cancelBtn.trailingAnchor constraintEqualToAnchor:div.leadingAnchor],
        [cancelBtn.heightAnchor constraintEqualToConstant:44],

        [div.centerXAnchor constraintEqualToAnchor:contentView.centerXAnchor],
        [div.topAnchor constraintEqualToAnchor:prevView.bottomAnchor],
        [div.bottomAnchor constraintEqualToAnchor:contentView.bottomAnchor],
        [div.widthAnchor constraintEqualToConstant:1 / [UIScreen mainScreen].scale],

        [launchBtn.topAnchor constraintEqualToAnchor:prevView.bottomAnchor],
        [launchBtn.trailingAnchor constraintEqualToAnchor:contentView.trailingAnchor],
        [launchBtn.leadingAnchor constraintEqualToAnchor:div.trailingAnchor],
        [launchBtn.heightAnchor constraintEqualToConstant:44],
    ]];

    // Ensure cancelBtn/launchBtn bottom connects to contentView bottom
    [NSLayoutConstraint activateConstraints:@[
        [cancelBtn.bottomAnchor constraintEqualToAnchor:contentView.bottomAnchor],
        [launchBtn.bottomAnchor constraintEqualToAnchor:contentView.bottomAnchor],
    ]];

    // Tap to dismiss keyboard
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(dismissKeyboard)];
    [vc.view addGestureRecognizer:tap];

    // Cancel: dismiss using block-based action
    __weak UIViewController *weakVC = vc;
    _BlockAction *cancelAction = [[_BlockAction alloc] init];
    cancelAction.block = ^{ [weakVC dismissViewControllerAnimated:YES completion:nil]; };
    [cancelBtn addTarget:cancelAction action:@selector(handleTap) forControlEvents:UIControlEventTouchUpInside];
    objc_setAssociatedObject(cancelBtn, "cancelAction", cancelAction, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    // Launch: dismiss and start game
    __weak FileBrowserViewController *weakSelf = self;
    _BlockAction *launchAction = [[_BlockAction alloc] init];
    launchAction.block = ^{
        NSString *text = argField.text ?: @"";
        [[NSUserDefaults standardUserDefaults] setBool:imSwitch.on forKey:imKey];
        [[NSUserDefaults standardUserDefaults] setObject:text forKey:[NSString stringWithFormat:@"launch_args_%@", gamedir]];
        if (botSwitch) {
            [[NSUserDefaults standardUserDefaults] setBool:botSwitch.on forKey:botsKey];
        }
        [[NSUserDefaults standardUserDefaults] synchronize];
        if (!imSwitch.on)
            text = [text stringByAppendingString:@" -noimmersive"];
        if (botSwitch && botSwitch.on && botDll.length > 0)
            text = [text stringByAppendingFormat:@" -dll %@", botDll];
        [vc dismissViewControllerAnimated:YES completion:^{
            [weakSelf doLaunchWithGameDir:gamedir extraArgs:text];
        }];
    };
    [launchBtn addTarget:launchAction action:@selector(handleTap) forControlEvents:UIControlEventTouchUpInside];
    objc_setAssociatedObject(launchBtn, "launchAction", launchAction, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    [self presentViewController:vc animated:YES completion:nil];
}

- (void)dismissKeyboard
{
    [self.view endEditing:YES];
}

- (void)doLaunchWithGameDir:(NSString *)gameDir extraArgs:(NSString *)extraArgs
{
    IOS_Log( "Xash: Launch button pressed" );
    {
        char buf[1024];
        snprintf( buf, sizeof(buf), "Xash: gameDir=%s extraArgs=%s", [gameDir UTF8String], [extraArgs UTF8String] );
        IOS_Log( buf );
    }

    // Allow landscape, then force orientation before launching the engine
    self.forceLandscape = YES;
    [UIViewController attemptRotationToDeviceOrientation];
    [self forceLandscapeOrientation];

    CGRect rect = [[UIScreen mainScreen] bounds];
    CGFloat scale = [UIScreen mainScreen].scale;

    NSMutableArray *argArray = [NSMutableArray arrayWithObject:@"xash"];
    if (extraArgs.length > 0)
        [argArray addObjectsFromArray:[extraArgs componentsSeparatedByString:@" "]];

    BOOL hasGame = NO;
    for (NSString *a in argArray)
        if ([a isEqualToString:@"-game"]) { hasGame = YES; break; }
    if (!hasGame) {
        [argArray addObject:@"-game"];
        [argArray addObject:gameDir];
    }

    // Build C-style argv
    int argc = (int)MIN(argArray.count, 63);
    static const char *c_args[64];
    static char c_storage[64][256];
    for (int i = 0; i < argc; i++) {
        strncpy(c_storage[i], [argArray[i] UTF8String], 255);
        c_storage[i][255] = '\0';
        c_args[i] = c_storage[i];
    }
    c_args[argc] = NULL;
    g_pszArgv = c_args;
    g_iArgc = argc;

    // Game libraries are pre-built and bundled in the app bundle
    // The engine will find them in the app's game directory
    IOS_Log( "Xash: starting engine with pre-built game libraries" );
    g_iStartGameStatus = XGS_START;
    IOS_Log( "Xash: g_iStartGameStatus set to XGS_START" );
}

- (void)forceLandscapeOrientation
{
    if (@available(iOS 16.0, *)) {
        UIWindowScene *scene = (UIWindowScene *)[[[UIApplication sharedApplication] connectedScenes] anyObject];
        if ([scene respondsToSelector:@selector(requestGeometryUpdateWithPreferences:errorHandler:)]) {
            UIWindowSceneGeometryPreferencesIOS *prefs = [[UIWindowSceneGeometryPreferencesIOS alloc] init];
            prefs.interfaceOrientations = UIInterfaceOrientationMaskLandscape;
            [scene requestGeometryUpdateWithPreferences:prefs errorHandler:nil];
        }
    } else {
        [[UIDevice currentDevice] setValue:@(UIInterfaceOrientationLandscapeLeft) forKey:@"orientation"];
    }
}

#pragma mark - File listing

- (void)reloadFiles
{
    NSArray *all = [self.fm contentsOfDirectoryAtPath:self.currentPath error:nil];
    NSMutableArray *dirs = [NSMutableArray array];
    NSMutableArray *fils = [NSMutableArray array];
    for (NSString *name in all) {
        if ([name hasPrefix:@"."]) continue;
        NSString *full = [self.currentPath stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        [self.fm fileExistsAtPath:full isDirectory:&isDir];
        if (isDir)
            [dirs addObject:name];
        else
            [fils addObject:name];
    }
    [dirs sortUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
    [fils sortUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
    self.directories = dirs;
    self.files = fils;
    [self.tableView reloadData];
}

- (NSString *)fullPathForIndexPath:(NSIndexPath *)ip
{
    NSString *name;
    if (ip.section == 0)
        name = self.directories[ip.row];
    else
        name = self.files[ip.row];
    return [self.currentPath stringByAppendingPathComponent:name];
}

- (NSDictionary *)attributesForPath:(NSString *)path
{
    return [self.fm attributesOfItemAtPath:path error:nil];
}

#pragma mark - Table view

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv
{
    return 2;
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)sec
{
    return sec == 0 ? self.directories.count : self.files.count;
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)sec
{
    if (sec == 0 && self.directories.count > 0) return @"Folders";
    if (sec == 1 && self.files.count > 0) return @"Files";
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
    UITableViewCell *cell = [tv dequeueReusableCellWithIdentifier:kCellID forIndexPath:ip];
    NSString *name = (ip.section == 0) ? self.directories[ip.row] : self.files[ip.row];
    NSString *full = [self.currentPath stringByAppendingPathComponent:name];
    NSDictionary *attr = [self attributesForPath:full];
    NSString *size = @"";
    if (ip.section == 1) {
        long long s = [attr fileSize];
        if (s < 1024) size = [NSString stringWithFormat:@"%lld B", s];
        else if (s < 1024 * 1024) size = [NSString stringWithFormat:@"%.1f KB", s / 1024.0];
        else size = [NSString stringWithFormat:@"%.1f MB", s / (1024.0 * 1024.0)];
    }
    cell.textLabel.text = name;
    cell.textLabel.font = [UIFont systemFontOfSize:16];
    if (ip.section == 0) {
        cell.imageView.image = [UIImage imageNamed:@"Folder"] ?: [self defaultFolderImage];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else {
        NSString *ext = name.pathExtension.lowercaseString;
        if ([@[@"cfg",@"txt",@"ini",@"log",@"rc"] containsObject:ext])
            cell.imageView.image = [self defaultTextImage];
        else
            cell.imageView.image = [self defaultFileImage];
        cell.accessoryType = UITableViewCellAccessoryNone;
    }
    if (ip.section == 1 && size.length > 0) {
        UILabel *subl = [[UILabel alloc] init];
        subl.text = size;
        subl.font = [UIFont systemFontOfSize:12];
        subl.textColor = [UIColor secondaryLabelColor];
        [subl sizeToFit];
        cell.accessoryView = subl;
    } else {
        cell.accessoryView = nil;
    }
    return cell;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [tv deselectRowAtIndexPath:ip animated:YES];
    if (ip.section == 0) {
        NSString *dir = self.directories[ip.row];
        NSString *full = [self.currentPath stringByAppendingPathComponent:dir];
        FileBrowserViewController *fbc = [[FileBrowserViewController alloc] initWithPath:full];
        [self.navigationController pushViewController:fbc animated:YES];
    } else {
        NSString *file = self.files[ip.row];
        NSString *full = [self.currentPath stringByAppendingPathComponent:file];
        [self openFile:full];
    }
}

#pragma mark - Swipe actions

- (UIContextMenuConfiguration *)tableView:(UITableView *)tv contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)ip point:(CGPoint)point
{
    NSString *full = [self fullPathForIndexPath:ip];
    BOOL isDir = (ip.section == 0);
    NSMutableArray *actions = [NSMutableArray array];

    if (!isDir) {
        [actions addObject:[UIAction actionWithTitle:@"Share/Export" image:[UIImage systemImageNamed:@"square.and.arrow.up"] identifier:nil handler:^(UIAction *a) {
            [self shareFile:full];
        }]];
    }

    [actions addObject:[UIAction actionWithTitle:@"Copy" image:[UIImage systemImageNamed:@"doc.on.doc"] identifier:nil handler:^(UIAction *a) {
        [self copyItem:full];
    }]];

    [actions addObject:[UIAction actionWithTitle:@"Rename" image:[UIImage systemImageNamed:@"pencil"] identifier:nil handler:^(UIAction *a) {
        [self renameItem:full];
    }]];

    [actions addObject:[UIAction actionWithTitle:@"Delete" image:[UIImage systemImageNamed:@"trash"] identifier:nil handler:^(UIAction *a) {
        [self deleteItem:full];
    }]];

    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil actionProvider:^UIMenu *(NSArray *suggested) {
        return [UIMenu menuWithTitle:[full lastPathComponent] children:actions];
    }];
}

#pragma mark - File operations

- (void)openFile:(NSString *)path
{
    NSString *ext = path.pathExtension.lowercaseString;
    NSSet *editable = [NSSet setWithObjects:@"txt", @"cfg", @"ini", @"log", @"rc", @"conf", nil];
    if ([editable containsObject:ext]) {
        TextEditorViewController *tec = [[TextEditorViewController alloc] initWithFilePath:path];
        UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:tec];
        nav.modalPresentationStyle = UIModalPresentationFullScreen;
        [self presentViewController:nav animated:YES completion:nil];
    } else {
        [self shareFile:path];
    }
}

- (void)shareFile:(NSString *)path
{
    NSURL *url = [NSURL fileURLWithPath:path];
    UIActivityViewController *avc = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        avc.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItem;
    }
    [self presentViewController:avc animated:YES completion:nil];
}

- (void)deleteItem:(NSString *)path
{
    NSString *name = [path lastPathComponent];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Delete" message:[NSString stringWithFormat:@"Delete \"%@\"?", name] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) {
        NSError *err = nil;
        [self.fm removeItemAtPath:path error:&err];
        if (err) [self showError:err];
        [self reloadFiles];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)renameItem:(NSString *)path
{
    NSString *name = [path lastPathComponent];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.text = name;
        tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
        NSString *newName = alert.textFields.firstObject.text;
        if (newName.length == 0 || [newName isEqualToString:name]) return;
        NSString *dest = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:newName];
        NSError *err = nil;
        [self.fm moveItemAtPath:path toPath:dest error:&err];
        if (err) [self showError:err];
        [self reloadFiles];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)copyItem:(NSString *)path
{
    _pasteboardPath = [path copy];
    _isMoveOperation = NO;
    [self updatePasteButton];
}

- (void)pasteTapped
{
    if (!_pasteboardPath) return;
    NSString *name = [_pasteboardPath lastPathComponent];
    NSString *dest = [self.currentPath stringByAppendingPathComponent:name];

    if ([self.fm fileExistsAtPath:dest]) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Overwrite?" message:[NSString stringWithFormat:@"\"%@\" already exists. Overwrite?", name] preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Skip" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
            _pasteboardPath = nil;
            [self updatePasteButton];
        }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Overwrite" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) {
            [self.fm removeItemAtPath:dest error:nil];
            [self doPaste:dest];
        }]];
        [self presentViewController:alert animated:YES completion:nil];
    } else {
        [self doPaste:dest];
    }
}

- (void)doPaste:(NSString *)dest
{
    NSError *err = nil;
    if (_isMoveOperation)
        [self.fm moveItemAtPath:_pasteboardPath toPath:dest error:&err];
    else
        [self.fm copyItemAtPath:_pasteboardPath toPath:dest error:&err];
    _pasteboardPath = nil;
    [self updatePasteButton];
    if (err) [self showError:err];
    [self reloadFiles];
}

- (void)updatePasteButton
{
    if (self.toolbarItems.count >= 7) {
        UIBarButtonItem *pasteBtn = self.toolbarItems[6];
        pasteBtn.enabled = (_pasteboardPath != nil);
        if (_isMoveOperation)
            pasteBtn.title = @"Move Here";
        else if (_pasteboardPath)
            pasteBtn.title = @"Paste";
        else
            pasteBtn.title = @"Paste";
    }
}

- (void)newFolderTapped
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"New Folder" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Folder name";
        tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Create" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
        NSString *name = alert.textFields.firstObject.text;
        if (name.length == 0) return;
        NSString *full = [self.currentPath stringByAppendingPathComponent:name];
        NSError *err = nil;
        [self.fm createDirectoryAtPath:full withIntermediateDirectories:NO attributes:nil error:&err];
        if (err) [self showError:err];
        [self reloadFiles];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)importTapped
{
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.data"] inMode:UIDocumentPickerModeImport];
    picker.delegate = self;
    picker.allowsMultipleSelection = YES;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    for (NSURL *url in urls) {
        [url startAccessingSecurityScopedResource];
        NSString *name = [[url lastPathComponent] stringByRemovingPercentEncoding];
        NSString *dest = [self.currentPath stringByAppendingPathComponent:name];
        [self.fm removeItemAtPath:dest error:nil];
        NSError *err = nil;
        [self.fm copyItemAtURL:url toURL:[NSURL fileURLWithPath:dest] error:&err];
        [url stopAccessingSecurityScopedResource];
        if (err) [self showError:err];
    }
    [self reloadFiles];
}

#pragma mark - Download Game Data (Steam CDN)

- (void)downloadDataTapped
{
    NSArray *gameKeys = @[@"valve", @"cstrike", @"tfc", @"gearbox", @"czero"];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Download Game Data" message:@"Select a game to download full game data from Steam CDN" preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSString *key in gameKeys) {
        NSString *title = [key capitalizedString];
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
            [self startDataDownloadForGame:key];
        }]];
    }

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        alert.popoverPresentationController.barButtonItem = self.toolbarItems[4];
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)startDataDownloadForGame:(NSString *)gameDir
{
    // Check if game data already exists (like Android hasGamedir)
    if ([self.dataDownloader hasGameData:gameDir]) {
        NSString *displayName = [gameDir capitalizedString];
        UIAlertController *overwriteAlert = [UIAlertController alertControllerWithTitle:@"Overwrite?" message:[NSString stringWithFormat:@"\"%@\" already exists. Overwrite?", displayName] preferredStyle:UIAlertControllerStyleAlert];
        __weak typeof(self) weakSelf = self;
        [overwriteAlert addAction:[UIAlertAction actionWithTitle:@"Skip" style:UIAlertActionStyleCancel handler:nil]];
        [overwriteAlert addAction:[UIAlertAction actionWithTitle:@"Overwrite" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) {
            [weakSelf beginDownloadForGame:gameDir];
        }]];
        [self presentViewController:overwriteAlert animated:YES completion:nil];
    } else {
        [self beginDownloadForGame:gameDir];
    }
}

- (void)beginDownloadForGame:(NSString *)gameDir
{
    UIAlertController *progressAlert = [UIAlertController alertControllerWithTitle:@"Downloading Game Data" message:[NSString stringWithFormat:@"Downloading %@...", gameDir] preferredStyle:UIAlertControllerStyleAlert];
    UIProgressView *progressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    progressView.translatesAutoresizingMaskIntoConstraints = NO;
    [progressAlert.view addSubview:progressView];
    [NSLayoutConstraint activateConstraints:@[
        [progressView.leadingAnchor constraintEqualToAnchor:progressAlert.view.leadingAnchor constant:16],
        [progressView.trailingAnchor constraintEqualToAnchor:progressAlert.view.trailingAnchor constant:-16],
        [progressView.bottomAnchor constraintEqualToAnchor:progressAlert.view.bottomAnchor constant:-48],
    ]];
    [progressAlert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:progressAlert animated:YES completion:nil];

    __weak typeof(self) weakSelf = self;
    [self.dataDownloader downloadGame:gameDir onProgress:^(NSString *status, float progress) {
        dispatch_async(dispatch_get_main_queue(), ^{
            progressAlert.message = status;
            progressView.progress = progress;
        });
    } completion:^(NSError *error) {
        [progressAlert dismissViewControllerAnimated:YES completion:^{
            if (error) {
                [weakSelf showError:error];
            } else {
                UIAlertController *done = [UIAlertController alertControllerWithTitle:@"Done" message:[NSString stringWithFormat:@"%@ game data downloaded successfully", gameDir] preferredStyle:UIAlertControllerStyleAlert];
                [done addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
                [weakSelf presentViewController:done animated:YES completion:nil];
            }
            [weakSelf reloadFiles];
        }];
    }];
}

#pragma mark - Helpers

- (void)showError:(NSError *)err
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Error" message:err.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (UIImage *)defaultFolderImage
{
    return [UIImage systemImageNamed:@"folder"];
}

- (UIImage *)defaultTextImage
{
    return [UIImage systemImageNamed:@"doc.text"];
}

- (UIImage *)defaultFileImage
{
    return [UIImage systemImageNamed:@"doc"];
}

#pragma mark - Orientation

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return self.forceLandscape ? UIInterfaceOrientationMaskLandscape : UIInterfaceOrientationMaskPortrait;
}

@end
