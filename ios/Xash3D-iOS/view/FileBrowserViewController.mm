#import "FileBrowserViewController.h"
#import "TextEditorViewController.h"
#import "GameDataDownloader.h"
#import "SteamService.h"
#include "SteamViewController.inc"
#include "launcherdialog.h"
@interface FileBrowserViewController () <UIDocumentPickerDelegate>
@property (nonatomic, strong) NSArray *directories;
@property (nonatomic, strong) NSArray *files;
@property (nonatomic, strong) NSFileManager *fm;
@property (nonatomic, assign) BOOL forceLandscape;
@property (nonatomic, strong) UIBarButtonItem *pasteButton;
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
        NSString *docsDir = [NSString stringWithUTF8String:IOS_GetDocsDir()];
        _dataDownloader = [[GameDataDownloader alloc] initWithDocumentsDir:docsDir];
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = [self.currentPath lastPathComponent];
    if (self.title.length == 0 || [self.currentPath isEqualToString:NSHomeDirectory()])
        self.title = @"SvenClient";

    // Back button (left) — only visible when not at root
    NSString *docs = [NSString stringWithUTF8String:IOS_GetDocsDir()];
    if (![self.currentPath isEqualToString:docs]) {
        UIBarButtonItem *backBtn = [[UIBarButtonItem alloc] initWithTitle:@"Back" style:UIBarButtonItemStylePlain target:self action:@selector(backTapped)];
        self.navigationItem.leftBarButtonItem = backBtn;
    }

    // Launch button (right)
    UIBarButtonItem *launchBtn = [[UIBarButtonItem alloc] initWithTitle:@"Play" style:UIBarButtonItemStyleDone target:self action:@selector(launchTapped)];
    self.navigationItem.rightBarButtonItems = @[
        launchBtn,
        [[UIBarButtonItem alloc] initWithTitle:@"Steam" style:UIBarButtonItemStylePlain target:self action:@selector(steamTapped)]
    ];
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"global_arguments": @"-console"}];
    if (SteamService.shared.autoConnect) [SteamService.shared restoreSession];

    UIBarButtonItem *importBtn = [[UIBarButtonItem alloc] initWithTitle:@"Import" style:UIBarButtonItemStylePlain target:self action:@selector(importTapped)];
    UIBarButtonItem *flex = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil];
    UIBarButtonItem *folderBtn = [[UIBarButtonItem alloc] initWithTitle:@"New Folder" style:UIBarButtonItemStylePlain target:self action:@selector(newFolderTapped)];
    UIBarButtonItem *pasteBtn = [[UIBarButtonItem alloc] initWithTitle:@"Paste" style:UIBarButtonItemStylePlain target:self action:@selector(pasteTapped)];
    self.pasteButton = pasteBtn;
    pasteBtn.enabled = (_pasteboardPath != nil);
    self.toolbarItems = @[folderBtn, flex, importBtn, flex, pasteBtn];
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

- (void)launchTapped
{
    if (self.dataDownloader.installing) {
        [self showError:[NSError errorWithDomain:@"SvenClient" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Finish or cancel game installation before playing."}]];
        return;
    }
    UIAlertController *launch = [UIAlertController alertControllerWithTitle:@"Command-line arguments" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [launch addTextFieldWithConfigurationHandler:^(UITextField *field){
        field.text = [[NSUserDefaults standardUserDefaults] stringForKey:@"global_arguments"] ?: @"-console";
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.autocorrectionType = UITextAutocorrectionTypeNo;
    }];
    [launch addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [launch addAction:[UIAlertAction actionWithTitle:@"Launch" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
        if (self.dataDownloader.installing) return;
        NSString *arguments = launch.textFields.firstObject.text ?: @"";
        [[NSUserDefaults standardUserDefaults] setObject:arguments forKey:@"global_arguments"];
        [SteamService.shared startBroker];
        [self doLaunchWithGameDir:@"svencoop" extraArgs:arguments];
    }]];
    [self presentViewController:launch animated:YES completion:nil];
}

- (void)steamTapped
{
    SteamViewController *steam = [[SteamViewController alloc] init]; steam.downloader = self.dataDownloader;
    [self.navigationController pushViewController:steam animated:YES];
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

    NSMutableArray *argArray = [NSMutableArray arrayWithObject:@"svenclient"];
    NSRegularExpression *pattern = [NSRegularExpression regularExpressionWithPattern:@"\"([^\"]*)\"|'([^']*)'|(\\S+)" options:0 error:nil];
    for (NSTextCheckingResult *match in [pattern matchesInString:extraArgs options:0 range:NSMakeRange(0, extraArgs.length)]) {
        for (NSUInteger group = 1; group <= 3; group++) {
            NSRange range = [match rangeAtIndex:group];
            if (range.location != NSNotFound) { [argArray addObject:[extraArgs substringWithRange:range]]; break; }
        }
    }
    for (NSUInteger index = 1; index < argArray.count;) {
        if ([argArray[index] isEqualToString:@"-game"]) {
            [argArray removeObjectAtIndex:index]; if (index < argArray.count) [argArray removeObjectAtIndex:index];
        } else index++;
    }
    [argArray addObjectsFromArray:@[@"-game", @"svencoop"]];
    if (argArray.count > 63) {
        [self showError:[NSError errorWithDomain:@"SvenClient" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Too many command-line arguments."}]]; return;
    }
    for (NSString *argument in argArray) if ([argument lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 255) {
        [self showError:[NSError errorWithDomain:@"SvenClient" code:1 userInfo:@{NSLocalizedDescriptionKey:@"A command-line argument is too long."}]]; return;
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
        fbc.dataDownloader = self.dataDownloader;
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
    self.pasteButton.enabled = (_pasteboardPath != nil);
    self.pasteButton.title = _pasteboardPath && _isMoveOperation ? @"Move Here" : @"Paste";
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
