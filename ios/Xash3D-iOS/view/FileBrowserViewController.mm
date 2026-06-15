#import "FileBrowserViewController.h"
#import "TextEditorViewController.h"
#import "GameLibDownloader.h"
#import "GameDataDownloader.h"

@interface FileBrowserViewController () <UIDocumentPickerDelegate, UIAlertViewDelegate>
@property (nonatomic, strong) NSArray *directories;
@property (nonatomic, strong) NSArray *files;
@property (nonatomic, strong) NSFileManager *fm;
@property (nonatomic, strong) NSString *pasteboardPath;
@property (nonatomic, assign) BOOL isMoveOperation;
@end

static NSString *kCellID = @"FileCell";

@implementation FileBrowserViewController

- (instancetype)initWithPath:(NSString *)path
{
    self = [super initWithStyle:UITableViewStylePlain];
    if (self) {
        _currentPath = [path copy];
        _fm = [NSFileManager defaultManager];
        NSString *docsDir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    _downloader = [[GameLibDownloader alloc] initWithDocumentsDir:docsDir];
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

    UIBarButtonItem *closeBtn = [[UIBarButtonItem alloc] initWithTitle:@"Close" style:UIBarButtonItemStyleDone target:self action:@selector(closeTapped)];
    self.navigationItem.leftBarButtonItem = closeBtn;

    UIBarButtonItem *importBtn = [[UIBarButtonItem alloc] initWithTitle:@"Import" style:UIBarButtonItemStylePlain target:self action:@selector(importTapped)];
    UIBarButtonItem *flex = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil];
    UIBarButtonItem *folderBtn = [[UIBarButtonItem alloc] initWithTitle:@"New Folder" style:UIBarButtonItemStylePlain target:self action:@selector(newFolderTapped)];
    UIBarButtonItem *downloadBtn = [[UIBarButtonItem alloc] initWithTitle:@"Download" style:UIBarButtonItemStylePlain target:self action:@selector(downloadTapped)];
    UIBarButtonItem *downloadDataBtn = [[UIBarButtonItem alloc] initWithTitle:@"Data" style:UIBarButtonItemStylePlain target:self action:@selector(downloadDataTapped)];
    UIBarButtonItem *pasteBtn = [[UIBarButtonItem alloc] initWithTitle:@"Paste" style:UIBarButtonItemStylePlain target:self action:@selector(pasteTapped)];
    pasteBtn.enabled = (self.pasteboardPath != nil);
    self.toolbarItems = @[importBtn, flex, folderBtn, flex, downloadBtn, flex, downloadDataBtn, flex, pasteBtn];
    self.navigationController.toolbarHidden = NO;

    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:kCellID];
    [self reloadFiles];
}

- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];
    self.navigationController.toolbarHidden = NO;
    [self reloadFiles];
}

- (void)closeTapped
{
    [self dismissViewControllerAnimated:YES completion:nil];
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
    self.pasteboardPath = path;
    self.isMoveOperation = NO;
    [self updatePasteButton];
}

- (void)pasteTapped
{
    if (!self.pasteboardPath) return;
    NSString *name = [self.pasteboardPath lastPathComponent];
    NSString *dest = [self.currentPath stringByAppendingPathComponent:name];

    if ([self.fm fileExistsAtPath:dest]) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Overwrite?" message:[NSString stringWithFormat:@"\"%@\" already exists. Overwrite?", name] preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Skip" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
            self.pasteboardPath = nil;
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
    if (self.isMoveOperation)
        [self.fm moveItemAtPath:self.pasteboardPath toPath:dest error:&err];
    else
        [self.fm copyItemAtPath:self.pasteboardPath toPath:dest error:&err];
    self.pasteboardPath = nil;
    [self updatePasteButton];
    if (err) [self showError:err];
    [self reloadFiles];
}

- (void)updatePasteButton
{
    if (self.toolbarItems.count >= 7) {
        UIBarButtonItem *pasteBtn = self.toolbarItems[6];
        pasteBtn.enabled = (self.pasteboardPath != nil);
        if (self.isMoveOperation)
            pasteBtn.title = @"Move Here";
        else if (self.pasteboardPath)
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

#pragma mark - Download

- (void)downloadTapped
{
    [self.downloader fetchManifestWithCompletion:^(NSDictionary *manifest, NSError *error) {
        if (error || !manifest) {
            [self showError:error ?: [NSError errorWithDomain:@"FileBrowser" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Failed to fetch manifest"}]];
            return;
        }
        [self showGameList:manifest];
    }];
}

- (void)showGameList:(NSDictionary *)manifest
{
    NSDictionary *mods = manifest[@"mods"];
    if (![mods isKindOfClass:[NSDictionary class]] || mods.count == 0) {
        [self showError:[NSError errorWithDomain:@"FileBrowser" code:0 userInfo:@{NSLocalizedDescriptionKey: @"No games available"}]];
        return;
    }

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Download Game" message:@"Select a game to download game libraries" preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSString *key in [[mods allKeys] sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]) {
        NSString *dl = [self.downloader isDownloaded:key] ? @" (downloaded)" : @"";
        NSString *title = [NSString stringWithFormat:@"%@%@", key, dl];
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
            [self startDownloadForGame:key];
        }]];
    }

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        alert.popoverPresentationController.barButtonItem = self.toolbarItems[4];
    }
    [self presentViewController:alert animated:YES completion:nil];
}

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
        alert.popoverPresentationController.barButtonItem = self.toolbarItems[6];
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)startDataDownloadForGame:(NSString *)gameDir
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

- (void)startDownloadForGame:(NSString *)gamedir
{
    UIAlertController *progressAlert = [UIAlertController alertControllerWithTitle:@"Downloading" message:[NSString stringWithFormat:@"Downloading %@...", gamedir] preferredStyle:UIAlertControllerStyleAlert];
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
    [self.downloader download:gamedir onProgress:^(float progress) {
        progressView.progress = progress;
        progressAlert.message = [NSString stringWithFormat:@"Downloading %@... %.0f%%", gamedir, progress * 100];
    } completion:^(NSError *error) {
        [progressAlert dismissViewControllerAnimated:YES completion:^{
            if (error) {
                [weakSelf showError:error];
            } else {
                UIAlertController *done = [UIAlertController alertControllerWithTitle:@"Done" message:[NSString stringWithFormat:@"%@ downloaded successfully", gamedir] preferredStyle:UIAlertControllerStyleAlert];
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

@end
