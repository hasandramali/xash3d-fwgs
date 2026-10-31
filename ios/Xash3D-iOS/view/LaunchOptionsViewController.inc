@interface LaunchOptionsViewController : UIViewController
@property (nonatomic, copy) NSString *arguments;
@property (nonatomic) BOOL immersive;
@property (nonatomic) BOOL microphone;
@property (nonatomic) BOOL allowEventResources;
@property (nonatomic, copy) void (^onLaunch)(NSString *, BOOL, BOOL, BOOL);
@end

@implementation LaunchOptionsViewController {
    UITextField *_argumentsField;
    UISwitch *_immersiveSwitch;
    UISwitch *_microphoneSwitch;
    UISwitch *_eventResourcesSwitch;
    UIScrollView *_scroll;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.preferredContentSize = CGSizeMake(420, 500);
    _scroll = [[UIScrollView alloc] init];
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
    _scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [self.view addSubview:_scroll];
    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 20;
    stack.translatesAutoresizingMaskIntoConstraints = NO; [_scroll addSubview:stack];
    UILabel *title = [[UILabel alloc] init];
    title.text = @"Command-line arguments"; title.numberOfLines = 0;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    [stack addArrangedSubview:title];
    _argumentsField = [[UITextField alloc] init];
    _argumentsField.text = self.arguments; _argumentsField.placeholder = @"-console";
    _argumentsField.borderStyle = UITextBorderStyleRoundedRect;
    _argumentsField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _argumentsField.autocorrectionType = UITextAutocorrectionTypeNo;
    [stack addArrangedSubview:_argumentsField];
    UIStackView *immersiveRow = [[UIStackView alloc] init];
    immersiveRow.axis = UILayoutConstraintAxisHorizontal; immersiveRow.spacing = 16;
    immersiveRow.alignment = UIStackViewAlignmentCenter;
    UILabel *immersiveLabel = [[UILabel alloc] init];
    immersiveLabel.text = @"Immersive mode";
    immersiveLabel.numberOfLines = 0;
    immersiveLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _immersiveSwitch = [[UISwitch alloc] init]; _immersiveSwitch.on = self.immersive;
    _immersiveSwitch.accessibilityLabel = @"Immersive mode";
    [_immersiveSwitch setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_immersiveSwitch setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    [immersiveRow addArrangedSubview:immersiveLabel]; [immersiveRow addArrangedSubview:_immersiveSwitch];
    [stack addArrangedSubview:immersiveRow];
    UIStackView *microphoneRow = [[UIStackView alloc] init];
    microphoneRow.axis = UILayoutConstraintAxisHorizontal; microphoneRow.spacing = 16;
    microphoneRow.alignment = UIStackViewAlignmentCenter;
    UILabel *microphoneLabel = [[UILabel alloc] init];
    microphoneLabel.text = @"Microphone";
    microphoneLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _microphoneSwitch = [[UISwitch alloc] init]; _microphoneSwitch.on = self.microphone;
    _microphoneSwitch.accessibilityLabel = @"Microphone";
    [_microphoneSwitch setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_microphoneSwitch setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    [microphoneRow addArrangedSubview:microphoneLabel]; [microphoneRow addArrangedSubview:_microphoneSwitch];
    [stack addArrangedSubview:microphoneRow];
    UIStackView *eventResourcesRow = [[UIStackView alloc] init];
    eventResourcesRow.axis = UILayoutConstraintAxisHorizontal; eventResourcesRow.spacing = 16;
    eventResourcesRow.alignment = UIStackViewAlignmentCenter;
    UILabel *eventResourcesLabel = [[UILabel alloc] init];
    eventResourcesLabel.text = @"Allow event resources";
    eventResourcesLabel.numberOfLines = 0;
    eventResourcesLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _eventResourcesSwitch = [[UISwitch alloc] init]; _eventResourcesSwitch.on = self.allowEventResources;
    _eventResourcesSwitch.accessibilityLabel = eventResourcesLabel.text;
    [_eventResourcesSwitch setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_eventResourcesSwitch setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    [eventResourcesRow addArrangedSubview:eventResourcesLabel]; [eventResourcesRow addArrangedSubview:_eventResourcesSwitch];
    [stack addArrangedSubview:eventResourcesRow];
    UIStackView *buttons = [[UIStackView alloc] init];
    buttons.axis = UILayoutConstraintAxisHorizontal; buttons.spacing = 24;
    buttons.distribution = UIStackViewDistributionFillEqually;
    UIButton *cancel = [UIButton buttonWithType:UIButtonTypeSystem];
    [cancel setTitle:@"Cancel" forState:UIControlStateNormal];
    [cancel addTarget:self action:@selector(cancelTapped) forControlEvents:UIControlEventTouchUpInside];
    UIButton *launch = [UIButton buttonWithType:UIButtonTypeSystem];
    [launch setTitle:@"Launch" forState:UIControlStateNormal];
    [launch addTarget:self action:@selector(launchTapped) forControlEvents:UIControlEventTouchUpInside];
    [buttons addArrangedSubview:cancel]; [buttons addArrangedSubview:launch]; [stack addArrangedSubview:buttons];
    [NSLayoutConstraint activateConstraints:@[
        [_scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [_scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [_scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [stack.leadingAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.trailingAnchor constant:-24],
        [stack.widthAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.widthAnchor constant:-48],
        [_argumentsField.heightAnchor constraintGreaterThanOrEqualToConstant:48],
        [immersiveRow.heightAnchor constraintGreaterThanOrEqualToConstant:44],
        [microphoneRow.heightAnchor constraintGreaterThanOrEqualToConstant:44],
        [eventResourcesRow.heightAnchor constraintGreaterThanOrEqualToConstant:44],
        [buttons.heightAnchor constraintGreaterThanOrEqualToConstant:44]
    ]];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(keyboardChanged:) name:UIKeyboardWillChangeFrameNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(keyboardChanged:) name:UIKeyboardWillHideNotification object:nil];
}
- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }
- (void)keyboardChanged:(NSNotification *)notification {
    CGRect keyboard = [self.view convertRect:[notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue] fromView:nil];
    CGRect overlap = CGRectIntersection(self.view.bounds, keyboard);
    CGFloat inset = CGRectIsNull(overlap) ? 0 : MAX(0, CGRectGetHeight(overlap) - self.view.safeAreaInsets.bottom);
    if ([notification.name isEqualToString:UIKeyboardWillHideNotification]) inset = 0;
    _scroll.contentInset = UIEdgeInsetsMake(0, 0, inset, 0);
    _scroll.scrollIndicatorInsets = _scroll.contentInset;
    [_scroll scrollRectToVisible:[_argumentsField convertRect:_argumentsField.bounds toView:_scroll] animated:YES];
}
- (void)cancelTapped { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)launchTapped {
    NSString *arguments = _argumentsField.text ?: @"";
    BOOL immersive = _immersiveSwitch.on;
    BOOL microphone = _microphoneSwitch.on;
    BOOL allowEventResources = _eventResourcesSwitch.on;
    void (^launch)(NSString *, BOOL, BOOL, BOOL) = self.onLaunch;
    [self.view endEditing:YES];
    [self dismissViewControllerAnimated:YES completion:^{ if (launch) launch(arguments, immersive, microphone, allowEventResources); }];
}
@end
