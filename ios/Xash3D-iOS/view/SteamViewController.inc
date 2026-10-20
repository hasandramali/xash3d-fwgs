#import "SteamService.h"

static __weak UIAlertController *svenLibraryAlert;
static NSUInteger svenLibraryAlertSession;
static BOOL svenLibraryPromptRetry;

static void SvenPresentLibraryPrompt(UIViewController *controller) {
    SteamService *steam = SteamService.shared;
    NSUInteger session = steam.svenLibrarySession;
    if (svenLibraryAlert && (!steam.loggedIn || svenLibraryAlertSession != session)) {
        [svenLibraryAlert dismissViewControllerAnimated:NO completion:nil]; svenLibraryAlert = nil;
    }
    if (!steam.loggedIn || !steam.svenLibraryPromptPending) return;
    UIViewController *presenter = controller.navigationController.visibleViewController ?: controller;
    if (!presenter.viewIfLoaded.window || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    if (svenLibraryAlert) return;
    while (presenter.presentedViewController && ![presenter.presentedViewController isKindOfClass:UIAlertController.class])
        presenter = presenter.presentedViewController;
    if (presenter.presentedViewController || presenter.isBeingPresented || presenter.isBeingDismissed) {
        if (!svenLibraryPromptRetry) {
            svenLibraryPromptRetry = YES;
            __weak UIViewController *weakController = controller;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
                svenLibraryPromptRetry = NO;
                UIViewController *strongController = weakController;
                if (strongController) SvenPresentLibraryPrompt(strongController);
            });
        }
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Sven Co-op library access"
        message:@"Sven Co-op must be in your Steam library to play online. Please open Steam & add Sven Co-op to your library for free, then reopen SvenClient."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
        [SteamService.shared acknowledgeSvenLibraryNotice:session];
    }]];
    svenLibraryAlert = alert; svenLibraryAlertSession = session;
    [presenter presentViewController:alert animated:YES completion:nil];
}

@interface SteamViewController : UIViewController
@property (nonatomic, strong) GameDataDownloader *downloader;
@end

@implementation SteamViewController {
    UITextField *_username, *_password, *_code;
    UILabel *_statusLabel, *_warning, *_installStatus;
    UIActivityIndicatorView *_activity;
    UIButton *_login, *_reconnect, *_cancel, *_logout, *_broker, *_install, *_cancelInstall;
    UISwitch *_autoConnect;
    UIProgressView *_progress;
}
- (UITextField *)field:(NSString *)placeholder secure:(BOOL)secure {
    UITextField *field = [[UITextField alloc] init];
    field.placeholder = placeholder; field.borderStyle = UITextBorderStyleRoundedRect;
    field.secureTextEntry = secure; field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    [field.heightAnchor constraintGreaterThanOrEqualToConstant:48].active = YES;
    return field;
}
- (UIButton *)button:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:title forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:48].active = YES;
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside]; return button;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = @"Steam"; self.view.backgroundColor = UIColor.systemBackgroundColor;
    UIScrollView *scroll = [[UIScrollView alloc] init]; scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive; [self.view addSubview:scroll];
    UIStackView *stack = [[UIStackView alloc] init]; stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO; [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-24],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48]
    ]];
    _statusLabel = [[UILabel alloc] init]; _statusLabel.numberOfLines = 0;
    _statusLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2]; [stack addArrangedSubview:_statusLabel];
    _activity = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _activity.hidesWhenStopped = YES; [stack addArrangedSubview:_activity];
    _username = [self field:@"Steam account name" secure:NO]; _username.textContentType = UITextContentTypeUsername;
    _username.text = SteamService.shared.accountName; [stack addArrangedSubview:_username];
    _password = [self field:@"Password" secure:YES]; _password.textContentType = UITextContentTypePassword; [stack addArrangedSubview:_password];
    _code = [self field:@"Steam Guard code" secure:NO]; _code.textContentType = UITextContentTypeOneTimeCode;
    _code.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters; [stack addArrangedSubview:_code];
    _warning = [[UILabel alloc] init]; _warning.numberOfLines = 0; _warning.textColor = UIColor.secondaryLabelColor;
    _warning.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _warning.text = @"We strongly recommend using Steam Guard in the Steam mobile app instead of email codes."; [stack addArrangedSubview:_warning];
    _login = [self button:@"Sign in" action:@selector(signIn)]; [stack addArrangedSubview:_login];
    _reconnect = [self button:@"Reconnect to Steam" action:@selector(reconnect)]; [stack addArrangedSubview:_reconnect];
    _cancel = [self button:@"Cancel sign-in" action:@selector(cancelSignIn)]; [stack addArrangedSubview:_cancel];
    _logout = [self button:@"Log out" action:@selector(logoutTapped)]; [stack addArrangedSubview:_logout];
    _broker = [self button:@"Start game connection" action:@selector(toggleBroker)]; [stack addArrangedSubview:_broker];
    UIStackView *autoRow = [[UIStackView alloc] init]; autoRow.axis = UILayoutConstraintAxisHorizontal; autoRow.spacing = 12;
    UILabel *autoLabel = [[UILabel alloc] init]; autoLabel.text = @"Connect to Steam automatically"; autoLabel.numberOfLines = 0;
    _autoConnect = [[UISwitch alloc] init]; _autoConnect.on = SteamService.shared.autoConnect;
    [_autoConnect addTarget:self action:@selector(autoChanged) forControlEvents:UIControlEventValueChanged];
    [autoRow addArrangedSubview:autoLabel]; [autoRow addArrangedSubview:_autoConnect]; [stack addArrangedSubview:autoRow];
    UILabel *privacy = [[UILabel alloc] init]; privacy.numberOfLines = 0; privacy.textColor = UIColor.secondaryLabelColor;
    privacy.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    privacy.text = @"Your password is sent encrypted to Steam and is never saved. Your saved session stays in this device’s Keychain."; [stack addArrangedSubview:privacy];
    NSString *installTitle = [self.downloader hasGameData:@"svencoop"] ? @"Reinstall game" : @"Install game";
    _install = [self button:installTitle action:@selector(installTapped)]; [stack addArrangedSubview:_install];
    _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault]; [stack addArrangedSubview:_progress];
    _installStatus = [[UILabel alloc] init]; _installStatus.numberOfLines = 0; _installStatus.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _installStatus.text = @"Steam account is not needed for downloading data."; [stack addArrangedSubview:_installStatus];
    _cancelInstall = [self button:@"Cancel installation" action:@selector(cancelInstall)]; [stack addArrangedSubview:_cancelInstall];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refresh) name:SvenSteamStateChanged object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refresh) name:UIApplicationDidBecomeActiveNotification object:nil];
    [self refresh];
}
- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated]; self.navigationController.toolbarHidden = YES; [self refresh];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated]; SvenPresentLibraryPrompt(self);
}
- (void)refresh {
    SteamService *steam = SteamService.shared;
    _statusLabel.text = steam.loggedIn ? [NSString stringWithFormat:@"%@\n%@", steam.accountName, steam.status] : steam.status;
    BOOL needsCode = [@[@"guard", @"email"] containsObject:steam.guardKind];
    _username.hidden = steam.loggedIn || needsCode; _password.hidden = steam.loggedIn || needsCode;
    _username.enabled = !steam.busy; _password.enabled = !steam.busy;
    _code.hidden = !needsCode; _code.enabled = !steam.busy;
    _code.placeholder = [steam.guardKind isEqualToString:@"email"] ? @"Email code" : @"Steam Guard code";
    _warning.hidden = ![steam.guardKind isEqualToString:@"email"];
    _login.hidden = steam.loggedIn; _login.enabled = !steam.busy;
    [_login setTitle:needsCode ? @"Verify code" : @"Sign in" forState:UIControlStateNormal];
    _cancel.hidden = steam.loggedIn || (!steam.busy && !steam.guardKind.length);
    _reconnect.hidden = steam.loggedIn || !steam.hasSavedSession || steam.guardKind.length;
    _reconnect.enabled = !steam.busy;
    _logout.hidden = !steam.loggedIn && !steam.hasSavedSession; _broker.hidden = !steam.loggedIn;
    [_broker setTitle:steam.brokerRunning ? @"Stop game connection" : @"Start game connection" forState:UIControlStateNormal];
    if (steam.busy) [_activity startAnimating]; else [_activity stopAnimating];
    if (steam.loggedIn) { _password.text = @""; _code.text = @""; }
    [_install setTitle:[self.downloader hasGameData:@"svencoop"] ? @"Reinstall game" : @"Install game" forState:UIControlStateNormal];
    _install.enabled = !self.downloader.installing; _cancelInstall.hidden = !self.downloader.installing;
    _progress.hidden = !self.downloader.installing;
    SvenPresentLibraryPrompt(self);
}
- (void)signIn {
    [self.view endEditing:YES];
    if ([SteamService.shared.guardKind isEqualToString:@"email"] || [SteamService.shared.guardKind isEqualToString:@"guard"]) {
        NSString *code = [_code.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (code.length) { [SteamService.shared submitCode:code]; _code.text = @""; }
    } else if (_username.text.length && _password.text.length) {
        [SteamService.shared login:_username.text password:_password.text]; _password.text = @"";
    }
}
- (void)cancelSignIn { [SteamService.shared cancelLogin]; }
- (void)reconnect { [SteamService.shared restoreSession]; }
- (void)autoChanged { SteamService.shared.autoConnect = _autoConnect.on; }
- (void)toggleBroker { if (SteamService.shared.brokerRunning) [SteamService.shared stopBroker]; else [SteamService.shared startBroker]; }
- (void)logoutTapped {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Log out?" message:@"Your saved Steam session will be removed from this device." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Logout" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action){ [SteamService.shared logout]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)installTapped {
    if ([self.downloader hasGameData:@"svencoop"]) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Reinstall Sven Co-op?" message:@"Game data will be downloaded again. Your other files are kept." preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Install" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){ [self beginInstall]; }]];
        [self presentViewController:alert animated:YES completion:nil];
    } else [self beginInstall];
}
- (void)beginInstall {
    __weak SteamViewController *weakSelf = self;
    [self.downloader downloadGame:@"svencoop" onProgress:^(NSString *status, float progress){
        dispatch_async(dispatch_get_main_queue(), ^{
            SteamViewController *strongSelf = weakSelf; if (!strongSelf) return;
            strongSelf->_installStatus.text = status; strongSelf->_progress.progress = progress;
        });
    } completion:^(NSError *error){
        SteamViewController *strongSelf = weakSelf; if (!strongSelf) return;
        strongSelf->_installStatus.text = error ? error.localizedDescription : @"Sven Co-op is ready to play."; [strongSelf refresh];
    }];
    [self refresh];
}
- (void)cancelInstall { [self.downloader cancel]; _installStatus.text = @"Cancelling installation…"; }
@end
