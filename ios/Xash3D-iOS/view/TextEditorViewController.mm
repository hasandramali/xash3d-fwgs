#import "TextEditorViewController.h"
@interface TextEditorViewController () <UITextViewDelegate>
@property (nonatomic, strong) UITextView *textView;
@property (nonatomic, strong) NSString *originalText;
@property (nonatomic, assign) BOOL hasChanges;
@end

static const NSInteger kMaxTextFileSize = 1024 * 1024;

@implementation TextEditorViewController

- (instancetype)initWithFilePath:(NSString *)path
{
    self = [super init];
    if (self) {
        _filePath = [path copy];
        self.title = [path lastPathComponent];
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    UIBarButtonItem *saveBtn = [[UIBarButtonItem alloc] initWithTitle:@"Save" style:UIBarButtonItemStyleDone target:self action:@selector(saveTapped)];
    UIBarButtonItem *cancelBtn = [[UIBarButtonItem alloc] initWithTitle:@"Cancel" style:UIBarButtonItemStylePlain target:self action:@selector(cancelTapped)];
    self.navigationItem.rightBarButtonItem = saveBtn;
    self.navigationItem.leftBarButtonItem = cancelBtn;

    NSFileManager *fm = [NSFileManager defaultManager];
    NSDictionary *attrs = [fm attributesOfItemAtPath:self.filePath error:nil];
    NSInteger fileSize = [attrs fileSize];

    if (fileSize > kMaxTextFileSize) {
        UILabel *label = [[UILabel alloc] init];
        label.text = [NSString stringWithFormat:@"File too large (%ld KB).\nMax: 1 MB", (long)(fileSize / 1024)];
        label.textAlignment = NSTextAlignmentCenter;
        label.numberOfLines = 0;
        label.textColor = [UIColor secondaryLabelColor];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
            [label.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
            [label.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.leadingAnchor constant:20],
            [label.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.trailingAnchor constant:-20],
        ]];
        return;
    }

    NSError *err = nil;
    NSString *content = [NSString stringWithContentsOfFile:self.filePath encoding:NSUTF8StringEncoding error:&err];
    if (!content) {
        content = [NSString stringWithContentsOfFile:self.filePath encoding:NSASCIIStringEncoding error:nil];
    }
    if (!content) {
        content = [NSString stringWithFormat:@"<< Unable to read file: %@ >>", err.localizedDescription ?: @"unknown error"];
        saveBtn.enabled = NO;
    }

    self.originalText = [content copy];
    self.textView = [[UITextView alloc] init];
    self.textView.text = content;
    self.textView.font = [UIFont fontWithName:@"Menlo" size:13] ?: [UIFont systemFontOfSize:13];
    self.textView.delegate = self;
    self.textView.translatesAutoresizingMaskIntoConstraints = NO;
    self.textView.autocorrectionType = UITextAutocorrectionTypeNo;
    self.textView.autocapitalizationType = UITextAutocapitalizationTypeNone;
    self.textView.spellCheckingType = UITextSpellCheckingTypeNo;
    [self.view addSubview:self.textView];

    [NSLayoutConstraint activateConstraints:@[
        [self.textView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [self.textView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [self.textView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.textView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    ]];

    [self.textView becomeFirstResponder];
}

- (void)textViewDidChange:(UITextView *)textView
{
    self.hasChanges = ![textView.text isEqualToString:self.originalText];
    self.navigationItem.rightBarButtonItem.enabled = self.hasChanges;
}

- (void)saveTapped
{
    NSError *err = nil;
    [self.textView.text writeToFile:self.filePath atomically:YES encoding:NSUTF8StringEncoding error:&err];
    if (err) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Save Failed" message:err.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }
    self.hasChanges = NO;
    self.navigationItem.rightBarButtonItem.enabled = NO;
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)cancelTapped
{
    if (self.hasChanges) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Discard Changes?" message:@"You have unsaved changes." preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Discard" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
            [self dismissViewControllerAnimated:YES completion:nil];
        }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Keep Editing" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }
    [self dismissViewControllerAnimated:YES completion:nil];
}

@end
