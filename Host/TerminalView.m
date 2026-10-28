#import <Foundation/Foundation.h>
#include <string.h>
#ifndef LSB_TERMINAL_SCREEN_TESTING
#import "TerminalView.h"
#endif

// A small screen for Fish's prompt and line editor. Cells preserve Unicode text, but do not implement wide-glyph layout.
@interface LSBTerminalScreen : NSObject
@property (nonatomic) NSUInteger rows, columns, row, column, cursorOffset;
@property (nonatomic) BOOL cursorVisible;
@property (nonatomic) BOOL bracketedPasteEnabled;
@property (nonatomic, copy) void (^reply)(NSData *data);
@property (nonatomic) BOOL presentationEnabled;
@property (nonatomic, copy) void (^presentationHandler)(void);
- (void)resizeRows:(NSUInteger)rows columns:(NSUInteger)columns;
- (void)consume:(NSData *)data;
- (NSString *)text;
- (NSString *)pasteSequenceForText:(NSString *)text;
@end

@implementation LSBTerminalScreen {
    NSMutableArray<NSMutableArray<NSString *> *> *_lines;
    NSMutableArray<NSString *> *_history;
    NSMutableString *_parameters;
    NSUInteger _savedRow, _savedColumn;
    unsigned _state, _remainingUTF8;
    uint32_t _codepoint, _minimumUTF8;
    BOOL _wrapPending;
    BOOL _presentationDirty, _presentationScheduled;
}

- (instancetype)init {
    if ((self = [super init])) {
        _lines = [NSMutableArray new];
        _history = [NSMutableArray new];
        _parameters = [NSMutableString new];
        _cursorVisible = YES;
        [self resizeRows:24 columns:80];
    }
    return self;
}

- (void)schedulePresentation {
    // Parsing and replies stay synchronous; output fragments only schedule one redraw on the next main-queue turn.
    if (!_presentationEnabled || !_presentationDirty || _presentationScheduled || !_presentationHandler) return;
    _presentationScheduled = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_presentationScheduled = NO;
        if (!self->_presentationEnabled || !self->_presentationDirty || !self->_presentationHandler) return;
        self->_presentationDirty = NO;
        self->_presentationHandler();
    });
}

- (void)setPresentationHandler:(void (^)(void))handler {
    _presentationHandler = [handler copy];
    [self schedulePresentation];
}

- (void)setPresentationEnabled:(BOOL)enabled {
    _presentationEnabled = enabled;
    if (enabled) [self schedulePresentation];
}

- (void)invalidatePresentation {
    _presentationDirty = YES;
    [self schedulePresentation];
}

- (NSMutableArray<NSString *> *)blankLine {
    NSMutableArray *line = [NSMutableArray arrayWithCapacity:self.columns];
    for (NSUInteger index = 0; index < self.columns; ++index) [line addObject:@" "];
    return line;
}

- (NSString *)lineText:(NSArray<NSString *> *)line {
    NSUInteger count = line.count;
    while (count && [line[count - 1] isEqualToString:@" "]) --count;
    return [[line subarrayWithRange:NSMakeRange(0, count)] componentsJoinedByString:@""];
}

- (void)rememberLine:(NSArray<NSString *> *)line {
    [_history addObject:[self lineText:line]];
    if (_history.count > 1000) [_history removeObjectsInRange:NSMakeRange(0, _history.count - 1000)];
}

- (void)resizeRows:(NSUInteger)rows columns:(NSUInteger)columns {
    rows = MAX(1, MIN(rows, 200));
    columns = MAX(1, MIN(columns, 400));
    self.columns = columns;
    while (_lines.count > rows) {
        if (self.row < rows) [_lines removeLastObject];
        else {
            [self rememberLine:_lines.firstObject];
            [_lines removeObjectAtIndex:0];
            --self.row;
        }
    }
    while (_lines.count < rows) [_lines addObject:[self blankLine]];
    for (NSMutableArray *line in _lines) {
        if (line.count > columns) [line removeObjectsInRange:NSMakeRange(columns, line.count - columns)];
        while (line.count < columns) [line addObject:@" "];
    }
    self.rows = rows;
    self.row = MIN(self.row, rows - 1);
    self.column = MIN(self.column, columns - 1);
    _wrapPending = NO;
    [self invalidatePresentation];
}

- (void)lineFeed {
    _wrapPending = NO;
    if (self.row + 1 < self.rows) ++self.row;
    else {
        [self rememberLine:_lines.firstObject];
        [_lines removeObjectAtIndex:0];
        [_lines addObject:[self blankLine]];
    }
}

- (void)putScalar:(uint32_t)value {
    unichar pair[2];
    NSUInteger length = 1;
    if (value > 0xffff) {
        value -= 0x10000;
        pair[0] = 0xd800 | (value >> 10);
        pair[1] = 0xdc00 | (value & 0x3ff);
        length = 2;
    } else pair[0] = (unichar)value;
    NSString *character = [NSString stringWithCharacters:pair length:length];
    if (length == 1 && [[NSCharacterSet nonBaseCharacterSet] characterIsMember:pair[0]] && (self.column || _wrapPending)) {
        NSUInteger column = _wrapPending ? self.column : self.column - 1;
        if (_lines[self.row][column].length < 32)
            _lines[self.row][column] = [_lines[self.row][column] stringByAppendingString:character];
        return;
    }
    if (_wrapPending) { self.column = 0; [self lineFeed]; }
    _lines[self.row][self.column] = character;
    if (self.column + 1 == self.columns) _wrapPending = YES;
    else ++self.column;
}

- (void)eraseLineFrom:(NSUInteger)first to:(NSUInteger)last row:(NSUInteger)row {
    for (NSUInteger index = first; index <= last && index < self.columns; ++index) _lines[row][index] = @" ";
}

- (void)sendReply:(NSString *)reply {
    if (self.reply) self.reply([reply dataUsingEncoding:NSUTF8StringEncoding]);
}

- (void)finishCSI:(uint8_t)command parameters:(NSString *)parameters {
    BOOL private = [parameters hasPrefix:@"?"];
    NSString *body = (private || [parameters hasPrefix:@">"]) ? [parameters substringFromIndex:1] : parameters;
    NSArray<NSString *> *parts = [body componentsSeparatedByString:@";"];
    NSInteger first = parts.count ? [parts[0] integerValue] : 0;
    NSInteger second = parts.count > 1 ? [parts[1] integerValue] : 0;
    NSUInteger amount = (NSUInteger)MAX(1, MIN(first, 10000));
    if (strchr("ABCDEFGH`dfJKPX@su", command)) _wrapPending = NO;
    switch (command) {
        case 'A': self.row -= MIN(self.row, amount); break;
        case 'B': self.row = MIN(self.rows - 1, self.row + amount); break;
        case 'C': self.column = MIN(self.columns - 1, self.column + amount); break;
        case 'D': self.column -= MIN(self.column, amount); break;
        case 'E': self.row = MIN(self.rows - 1, self.row + amount); self.column = 0; break;
        case 'F': self.row -= MIN(self.row, amount); self.column = 0; break;
        case 'G': case '`': self.column = MIN(self.columns - 1, amount - 1); break;
        case 'd': self.row = MIN(self.rows - 1, amount - 1); break;
        case 'H': case 'f':
            self.row = MIN(self.rows - 1, amount - 1);
            self.column = MIN(self.columns - 1, (NSUInteger)MAX(1, second) - 1);
            break;
        case 'K':
            [self eraseLineFrom:first == 0 ? self.column : 0 to:first == 1 ? self.column : self.columns - 1 row:self.row];
            break;
        case 'J':
            if (first == 3) [_history removeAllObjects];
            else if (first == 2) {
                for (NSUInteger row = 0; row < self.rows; ++row) [self eraseLineFrom:0 to:self.columns - 1 row:row];
            } else if (first == 0) {
                [self eraseLineFrom:self.column to:self.columns - 1 row:self.row];
                for (NSUInteger row = self.row + 1; row < self.rows; ++row) [self eraseLineFrom:0 to:self.columns - 1 row:row];
            } else if (first == 1) {
                for (NSUInteger row = 0; row < self.row; ++row) [self eraseLineFrom:0 to:self.columns - 1 row:row];
                [self eraseLineFrom:0 to:self.column row:self.row];
            }
            break;
        case 'P': case '@': {
            NSMutableArray *line = _lines[self.row];
            amount = MIN(amount, self.columns - self.column);
            for (NSUInteger index = 0; index < amount; ++index) {
                if (command == 'P') { [line removeObjectAtIndex:self.column]; [line addObject:@" "]; }
                else { [line insertObject:@" " atIndex:self.column]; [line removeLastObject]; }
            }
            break;
        }
        case 'X': [self eraseLineFrom:self.column to:MIN(self.columns - 1, self.column + amount - 1) row:self.row]; break;
        case 's': _savedRow = self.row; _savedColumn = self.column; break;
        case 'u': self.row = MIN(_savedRow, self.rows - 1); self.column = MIN(_savedColumn, self.columns - 1); break;
        case 'n':
            if (first == 6) [self sendReply:[NSString stringWithFormat:@"\033[%lu;%luR", self.row + 1, self.column + 1]];
            else if (first == 5) [self sendReply:@"\033[0n"];
            break;
        case 'c':
            // Responses may be echoed by the line discipline. Only answer requests, never echoed DA responses.
            if (!parameters.length || [parameters isEqualToString:@"0"]) [self sendReply:@"\033[?1;2c"];
            else if ([parameters isEqualToString:@">"] || [parameters isEqualToString:@">0"]) [self sendReply:@"\033[>0;1;0c"];
            break;
        case 'h': case 'l':
            if (private) for (NSString *part in parts) {
                if (part.integerValue == 25) self.cursorVisible = command == 'h';
                else if (part.integerValue == 2004) self.bracketedPasteEnabled = command == 'h';
            }
            break;
        default: break; // Colors and unsupported modes have no visual effect.
    }
}

- (void)consumeByte:(uint8_t)byte {
    // 0=ground, 1=ESC, 2=CSI, 3=OSC, 4=OSC escape, 5=DCS/string, 6=string escape, 7=charset.
    if (_state == 3 || _state == 5) {
        if (byte == 0x1b) ++_state;
        else if (_state == 3 && byte == 7) _state = 0;
        return;
    }
    if (_state == 4 || _state == 6) {
        _state = byte == '\\' ? 0 : (_state == 4 ? 3 : 5);
        return;
    }
    if (_state == 7) { _state = 0; return; }
    if (_state == 2) {
        if (byte >= 0x40 && byte <= 0x7e) {
            NSString *parameters = [_parameters copy];
            [_parameters setString:@""];
            _state = 0;
            // Sending a reply can synchronously echo bytes back into consume:, including a new partial escape.
            [self finishCSI:byte parameters:parameters];
        }
        else if (byte == 0x1b) _state = 1;
        else if (byte >= 0x20 && byte <= 0x3f && _parameters.length < 128) [_parameters appendFormat:@"%c", byte];
        return;
    }
    if (_state == 1) {
        _state = 0;
        switch (byte) {
            case '[': _state = 2; [_parameters setString:@""]; break;
            case ']': _state = 3; break;
            case 'P': case '^': case '_': _state = 5; break;
            case '(': case ')': case '*': case '+': _state = 7; break;
            case '7': _savedRow = self.row; _savedColumn = self.column; break;
            case '8': self.row = MIN(_savedRow, self.rows - 1); self.column = MIN(_savedColumn, self.columns - 1); _wrapPending = NO; break;
            case 'D': [self lineFeed]; break;
            case 'E': self.column = 0; [self lineFeed]; break;
            case 'M':
                if (self.row) --self.row;
                else { [_lines insertObject:[self blankLine] atIndex:0]; [_lines removeLastObject]; }
                break;
            case 'c':
                [_history removeAllObjects];
                for (NSUInteger row = 0; row < self.rows; ++row) [self eraseLineFrom:0 to:self.columns - 1 row:row];
                self.row = self.column = 0; _wrapPending = NO; self.cursorVisible = YES; self.bracketedPasteEnabled = NO;
                break;
            default: break;
        }
        return;
    }
    if (_remainingUTF8) {
        if ((byte & 0xc0) == 0x80) {
            _codepoint = (_codepoint << 6) | (byte & 0x3f);
            if (!--_remainingUTF8) {
                BOOL valid = _codepoint >= _minimumUTF8 && _codepoint <= 0x10ffff && !(_codepoint >= 0xd800 && _codepoint <= 0xdfff);
                [self putScalar:valid ? _codepoint : 0xfffd];
            }
            return;
        }
        _remainingUTF8 = 0;
        [self putScalar:0xfffd];
    }
    if (byte == 0x1b) { _state = 1; return; }
    if (byte == '\r') { self.column = 0; _wrapPending = NO; return; }
    if (byte == '\n' || byte == '\v' || byte == '\f') { [self lineFeed]; return; }
    if (byte == '\b') { if (self.column) --self.column; _wrapPending = NO; return; }
    if (byte == '\t') { self.column = MIN(self.columns - 1, (self.column / 8 + 1) * 8); _wrapPending = NO; return; }
    if (byte < 0x20 || byte == 0x7f) return;
    if (byte < 0x80) { [self putScalar:byte]; return; }
    if (byte >= 0xc2 && byte <= 0xdf) { _remainingUTF8 = 1; _codepoint = byte & 0x1f; _minimumUTF8 = 0x80; }
    else if (byte >= 0xe0 && byte <= 0xef) { _remainingUTF8 = 2; _codepoint = byte & 0x0f; _minimumUTF8 = 0x800; }
    else if (byte >= 0xf0 && byte <= 0xf4) { _remainingUTF8 = 3; _codepoint = byte & 7; _minimumUTF8 = 0x10000; }
    else [self putScalar:0xfffd];
}

- (void)consume:(NSData *)data {
    const uint8_t *bytes = data.bytes;
    for (NSUInteger index = 0; index < data.length; ++index) [self consumeByte:bytes[index]];
    if (data.length) [self invalidatePresentation];
}

- (NSString *)text {
    NSMutableString *text = [NSMutableString new];
    for (NSString *line in _history) [text appendFormat:@"%@\n", line];
    NSUInteger last = self.row;
    for (NSUInteger row = self.row + 1; row < self.rows; ++row) if ([self lineText:_lines[row]].length) last = row;
    for (NSUInteger row = 0; row <= last; ++row) {
        NSString *line = [self lineText:_lines[row]];
        if (row == self.row) {
            NSUInteger prefix = 0;
            for (NSUInteger index = 0; index < self.column; ++index) prefix += _lines[row][index].length;
            while (line.length <= prefix) line = [line stringByAppendingString:@" "];
            self.cursorOffset = text.length + prefix;
        }
        [text appendString:line];
        if (row < last) [text appendString:@"\n"];
    }
    return text;
}

- (NSString *)pasteSequenceForText:(NSString *)text {
    if (!text.length) return @"";
    if (self.bracketedPasteEnabled) return [NSString stringWithFormat:@"\033[200~%@\033[201~", text];
    return [text stringByReplacingOccurrencesOfString:@"\n" withString:@"\r"];
}
@end

#ifndef LSB_TERMINAL_SCREEN_TESTING
@interface LSBTerminalView () <UIGestureRecognizerDelegate>
- (void)hardwareKey:(UIKeyCommand *)command;
@end

// UIKit and keyboard extensions get a real UITextInput delegate; the terminal screen remains the sole visible editor.
@interface LSBTerminalInputField : UITextField <UITextFieldDelegate>
@property (nonatomic, weak) LSBTerminalView *terminal;
@end

@implementation LSBTerminalInputField {
    unsigned _nativeEditingDepth;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.delegate = self;
        self.borderStyle = UITextBorderStyleNone;
        self.backgroundColor = self.textColor = self.tintColor = UIColor.clearColor;
        self.clipsToBounds = YES;
        self.isAccessibilityElement = NO;
        [self addTarget:self action:@selector(commitTextFromField) forControlEvents:UIControlEventEditingChanged];
    }
    return self;
}
- (BOOL)acceptsTerminalInput { return self.terminal.inputHandler != nil && self.terminal.presentationEnabled; }
- (BOOL)canBecomeFirstResponder { return [self acceptsTerminalInput] && [super canBecomeFirstResponder]; }
- (BOOL)hasText { return [self.terminal hasText]; }
- (void)drawTextInRect:(CGRect)rect { (void)rect; }
- (void)drawPlaceholderInRect:(CGRect)rect { (void)rect; }
- (UIEditingInteractionConfiguration)editingInteractionConfiguration { return self.terminal.editingInteractionConfiguration; }
- (NSArray<UIKeyCommand *> *)keyCommands { return self.terminal.keyCommands; }
- (void)hardwareKey:(UIKeyCommand *)command { if ([self acceptsTerminalInput]) [self.terminal hardwareKey:command]; }
- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(paste:) || action == @selector(hardwareKey:)) return [self acceptsTerminalInput];
    return NO; // The proxy has no independently editable command line for Cut, Copy or Select All.
}
- (void)insertText:(NSString *)text {
    if (_nativeEditingDepth) { [super insertText:text]; return; }
    if (self.markedTextRange) {
        ++_nativeEditingDepth; [super insertText:text]; --_nativeEditingDepth;
        [self commitTextFromField];
    }
    else if ([self acceptsTerminalInput]) [self.terminal insertText:text];
}
- (void)deleteBackward {
    if (_nativeEditingDepth) { [super deleteBackward]; return; }
    if (self.markedTextRange) {
        ++_nativeEditingDepth; [super deleteBackward]; --_nativeEditingDepth;
        [self commitTextFromField];
    }
    else if ([self acceptsTerminalInput]) [self.terminal deleteBackward];
}
- (void)replaceRange:(UITextRange *)range withText:(NSString *)text {
    ++_nativeEditingDepth; [super replaceRange:range withText:text]; --_nativeEditingDepth;
    [self commitTextFromField];
}
- (void)setMarkedText:(NSString *)markedText selectedRange:(NSRange)selectedRange {
    ++_nativeEditingDepth; [super setMarkedText:markedText selectedRange:selectedRange]; --_nativeEditingDepth;
}
- (void)commitTextFromField {
    // UIKit can commit through text replacement instead of UIKeyInput. Keep its marked text local until it is committed.
    if (_nativeEditingDepth || self.markedTextRange) return;
    NSString *text = [self.text copy];
    if (!text.length) return;
    self.text = @"";
    if ([self acceptsTerminalInput]) [self.terminal insertText:text];
}
- (void)unmarkText {
    ++_nativeEditingDepth; [super unmarkText]; --_nativeEditingDepth;
    [self commitTextFromField];
}
- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    (void)textField;
    if ([self acceptsTerminalInput]) [self.terminal insertText:@"\n"];
    return NO;
}
- (void)paste:(id)sender { if ([self acceptsTerminalInput]) [self.terminal paste:sender]; }
- (BOOL)canPasteItemProviders:(NSArray<NSItemProvider *> *)providers {
    return [self acceptsTerminalInput] && [self.terminal canPasteItemProviders:providers];
}
- (void)pasteItemProviders:(NSArray<NSItemProvider *> *)providers {
    if ([self acceptsTerminalInput]) [self.terminal pasteItemProviders:providers];
}
@end

@implementation LSBTerminalView {
    UITextView *_display;
    LSBTerminalInputField *_inputField;
    UILabel *_status;
    UIScrollView *_controls;
    LSBTerminalScreen *_screen;
    UIFont *_font;
    CGFloat _cellWidth, _cellHeight;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = UIColor.blackColor;
        self.accessibilityLabel = @"Simulator terminal";
        self.pasteConfiguration = [[UIPasteConfiguration alloc] initWithTypeIdentifiersForAcceptingClass:NSString.class];
        _font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
        _cellWidth = [@"M" sizeWithAttributes:@{NSFontAttributeName: _font}].width;
        _cellHeight = _font.lineHeight;
        _screen = [LSBTerminalScreen new];
        __weak LSBTerminalView *weakSelf = self;
        _screen.reply = ^(NSData *data) { if (weakSelf.inputHandler) weakSelf.inputHandler(data); };
        _display = [[UITextView alloc] initWithFrame:self.bounds];
        _display.backgroundColor = UIColor.blackColor;
        _display.textColor = UIColor.whiteColor;
        _display.font = _font;
        _display.editable = NO;
        _display.selectable = NO;
        // The real keyboard responder lives inside this scroll view, preserving interactive keyboard dismissal.
        _display.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
        _display.alwaysBounceVertical = YES;
        _display.textContainerInset = UIEdgeInsetsMake(8, 8, 8, 8);
        _display.textContainer.lineFragmentPadding = 0;
        _display.textContainer.lineBreakMode = NSLineBreakByClipping;
        _display.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [self addSubview:_display];
        _inputField = [[LSBTerminalInputField alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
        _inputField.terminal = self;
        _inputField.pasteConfiguration = self.pasteConfiguration;
        _inputField.keyboardType = self.keyboardType;
        _inputField.keyboardAppearance = self.keyboardAppearance;
        _inputField.autocorrectionType = self.autocorrectionType;
        _inputField.autocapitalizationType = self.autocapitalizationType;
        _inputField.spellCheckingType = self.spellCheckingType;
        _inputField.smartQuotesType = self.smartQuotesType;
        _inputField.smartDashesType = self.smartDashesType;
        _inputField.smartInsertDeleteType = self.smartInsertDeleteType;
        [_display addSubview:_inputField];
        _status = [UILabel new];
        _status.textColor = UIColor.lightGrayColor;
        _status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
        _status.adjustsFontForContentSizeCategory = YES;
        _status.numberOfLines = 2;
        [self addSubview:_status];
        UITapGestureRecognizer *focus = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(focusTerminal:)];
        focus.delegate = self;
        [self addGestureRecognizer:focus];
        [self buildControls];
        _screen.presentationHandler = ^{ [weakSelf render]; };
    }
    return self;
}

- (void)buildControls {
    // Keep shortcuts inside the terminal's safe-area/keyboard constraints. A system inputAccessoryView can sit under a floating tab bar.
    UIScrollView *controls = [[UIScrollView alloc] initWithFrame:CGRectMake(0, 0, 400, 44)];
    controls.backgroundColor = [UIColor colorWithWhite:0.08 alpha:1];
    controls.userInteractionEnabled = NO;
    controls.alpha = 0.5;
    controls.pagingEnabled = YES;
    controls.directionalLockEnabled = YES;
    controls.showsVerticalScrollIndicator = NO;
    controls.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    controls.accessibilityLabel = @"Terminal shortcuts";
    NSArray *labels = @[@"Esc", @"Tab", @"^C", @"^D", @"←", @"↓", @"↑", @"→", @"↵", @"Paste"];
    UIStackView *previous = nil;
    for (NSArray<NSNumber *> *indices in @[@[@0, @1, @2, @3, @8, @9], @[@4, @5, @6, @7]]) {
        UIStackView *page = [UIStackView new];
        page.translatesAutoresizingMaskIntoConstraints = NO;
        page.distribution = UIStackViewDistributionFillEqually;
        for (NSNumber *index in indices) {
            UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
            [button setTitle:labels[index.unsignedIntegerValue] forState:UIControlStateNormal];
            button.titleLabel.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightMedium];
            button.tag = index.integerValue;
            // Try Paste on demand even before UIKit has discovered clipboard text, including a remote clipboard.
            [button addTarget:self action:index.unsignedIntegerValue == 9 ? @selector(paste:) : @selector(controlPressed:)
                forControlEvents:UIControlEventTouchUpInside];
            [page addArrangedSubview:button];
        }
        [controls addSubview:page];
        [NSLayoutConstraint activateConstraints:@[
            [page.leadingAnchor constraintEqualToAnchor:previous ? previous.trailingAnchor : controls.contentLayoutGuide.leadingAnchor],
            [page.topAnchor constraintEqualToAnchor:controls.contentLayoutGuide.topAnchor],
            [page.bottomAnchor constraintEqualToAnchor:controls.contentLayoutGuide.bottomAnchor],
            [page.widthAnchor constraintEqualToAnchor:controls.frameLayoutGuide.widthAnchor],
            [page.heightAnchor constraintEqualToAnchor:controls.frameLayoutGuide.heightAnchor]
        ]];
        previous = page;
    }
    [previous.trailingAnchor constraintEqualToAnchor:controls.contentLayoutGuide.trailingAnchor].active = YES;
    [self addSubview:controls];
    _controls = controls;
}

- (BOOL)canBecomeFirstResponder { return self.inputHandler != nil && self.presentationEnabled; }
- (BOOL)becomeFirstResponder {
    if (![self canBecomeFirstResponder]) return NO;
    [self layoutIfNeeded];
    [self render]; // Place the real responder at the current terminal caret before UIKit tries to reveal it.
    return [_inputField becomeFirstResponder];
}
- (BOOL)resignFirstResponder { return [_inputField resignFirstResponder]; }
- (BOOL)isFirstResponder { return _inputField.isFirstResponder; }
- (UIEditingInteractionConfiguration)editingInteractionConfiguration {
    // Use UIKit's productivity gestures with this responder's standard Paste action; the output view stays read-only.
    return self.inputHandler ? UIEditingInteractionConfigurationDefault : UIEditingInteractionConfigurationNone;
}
- (BOOL)hasText { return YES; }
- (UIKeyboardType)keyboardType { return UIKeyboardTypeASCIICapable; }
- (UIKeyboardAppearance)keyboardAppearance { return UIKeyboardAppearanceDark; }
- (UITextAutocorrectionType)autocorrectionType { return UITextAutocorrectionTypeNo; }
- (UITextAutocapitalizationType)autocapitalizationType { return UITextAutocapitalizationTypeNone; }
- (UITextSpellCheckingType)spellCheckingType { return UITextSpellCheckingTypeNo; }
- (UITextSmartQuotesType)smartQuotesType { return UITextSmartQuotesTypeNo; }
- (UITextSmartDashesType)smartDashesType { return UITextSmartDashesTypeNo; }
- (UITextSmartInsertDeleteType)smartInsertDeleteType { return UITextSmartInsertDeleteTypeNo; }
- (void)focusTerminal:(UITapGestureRecognizer *)gesture { (void)gesture; [self becomeFirstResponder]; }
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gesture shouldReceiveTouch:(UITouch *)touch {
    (void)gesture;
    // Ancestor tap recognizers cancel descendant touches by default. Leave Paste, shortcut buttons and their paging scroll alone.
    return ![touch.view isDescendantOfView:_controls];
}

- (void)sendString:(NSString *)text {
    if (self.inputHandler) self.inputHandler([text dataUsingEncoding:NSUTF8StringEncoding]);
}

- (void)insertText:(NSString *)text { [self sendString:[text stringByReplacingOccurrencesOfString:@"\n" withString:@"\r"]]; }
- (void)deleteBackward { [self sendString:@"\177"]; }

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(paste:)) return self.inputHandler != nil;
    return [super canPerformAction:action withSender:sender];
}

- (BOOL)canPasteItemProviders:(NSArray<NSItemProvider *> *)providers {
    if (!self.inputHandler) return NO;
    for (NSItemProvider *provider in providers) if ([provider canLoadObjectOfClass:NSString.class]) return YES;
    return NO;
}

- (void)paste:(id)sender {
    (void)sender;
    // The standard Paste action is an explicit user gesture; no clipboard content is read while rendering or updating controls.
    if (self.inputHandler) [self pasteItemProviders:UIPasteboard.generalPasteboard.itemProviders];
}

- (void)pasteItemProviders:(NSArray<NSItemProvider *> *)providers {
    void (^handler)(NSData *) = self.inputHandler;
    if (!handler) return;
    __weak LSBTerminalView *weakSelf = self;
    for (NSItemProvider *provider in providers) {
        if (![provider canLoadObjectOfClass:NSString.class]) continue;
        [provider loadObjectOfClass:NSString.class completionHandler:^(id<NSItemProviderReading> object, NSError *error) {
            NSString *text = !error && [object isKindOfClass:NSString.class] ? [(NSString *)object copy] : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                LSBTerminalView *terminal = weakSelf;
                // A delayed clipboard load must not enter a different Fish session or a tab that was dismissed meanwhile.
                if (text.length && terminal.inputHandler == handler && terminal.presentationEnabled)
                    [terminal sendString:[terminal->_screen pasteSequenceForText:text]];
            });
        }];
        break; // The text representation of the first supported clipboard item is the terminal paste.
    }
}

- (void)controlPressed:(UIButton *)button {
    NSArray *sequences = @[@"\033", @"\t", @"\003", @"\004", @"\033[D", @"\033[B", @"\033[A", @"\033[C", @"\r"];
    if ((NSUInteger)button.tag < sequences.count) [self sendString:sequences[(NSUInteger)button.tag]];
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    NSMutableArray *commands = [NSMutableArray new];
    [commands addObject:[UIKeyCommand keyCommandWithInput:@"v" modifierFlags:UIKeyModifierCommand action:@selector(paste:)]];
    for (NSString *input in @[UIKeyInputUpArrow, UIKeyInputDownArrow, UIKeyInputLeftArrow, UIKeyInputRightArrow, @"\033", @"\t"]) {
        UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:input modifierFlags:0 action:@selector(hardwareKey:)];
        command.wantsPriorityOverSystemBehavior = YES;
        [commands addObject:command];
    }
    for (NSString *input in @[@"a", @"b", @"c", @"d", @"e", @"f", @"k", @"l", @"n", @"p", @"r", @"u", @"v", @"w", @"z", @"\\"]) {
        UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:input modifierFlags:UIKeyModifierControl action:@selector(hardwareKey:)];
        command.wantsPriorityOverSystemBehavior = YES;
        [commands addObject:command];
    }
    return commands;
}

- (void)hardwareKey:(UIKeyCommand *)command {
    NSDictionary *arrows = @{UIKeyInputUpArrow: @"\033[A", UIKeyInputDownArrow: @"\033[B", UIKeyInputRightArrow: @"\033[C",
                             UIKeyInputLeftArrow: @"\033[D"};
    NSString *sequence = arrows[command.input];
    if (command.modifierFlags & UIKeyModifierControl) {
        unichar value = [command.input characterAtIndex:0] & 0x1f;
        sequence = [NSString stringWithCharacters:&value length:1];
    }
    [self sendString:sequence ?: command.input];
}

- (void)render {
    NSString *text = [_screen text];
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:@{
        NSFontAttributeName: _font, NSForegroundColorAttributeName: UIColor.whiteColor
    }];
    if (_screen.cursorVisible && _screen.cursorOffset < styled.length)
        [styled addAttribute:NSBackgroundColorAttributeName value:UIColor.darkGrayColor range:NSMakeRange(_screen.cursorOffset, 1)];
    BOOL changed = ![_display.attributedText isEqualToAttributedString:styled];
    [UIView performWithoutAnimation:^{
        // Update TextKit's existing storage in one edit instead of resetting the text view's attributedText property.
        if (changed) {
            NSTextStorage *storage = self->_display.textStorage;
            [storage beginEditing];
            [storage setAttributedString:styled];
            [storage endEditing];
        }
        // Resizing or moving a hidden cursor can require scrolling without changing the attributed text.
        [self->_display layoutIfNeeded];
        UITextPosition *cursor = [self->_display positionFromPosition:self->_display.beginningOfDocument
            offset:MIN(self->_screen.cursorOffset, styled.length)];
        if (cursor) {
            CGRect caret = [self->_display caretRectForPosition:cursor];
            self->_inputField.frame = CGRectMake(caret.origin.x, caret.origin.y, 1, 1);
            if (!CGRectContainsRect(self->_display.bounds, caret)) [self->_display scrollRectToVisible:caret animated:NO];
        }
    }];
}

- (void)appendOutput:(NSData *)data {
    if (![NSThread isMainThread]) {
        NSData *copy = [data copy];
        dispatch_async(dispatch_get_main_queue(), ^{ [self appendOutput:copy]; });
        return;
    }
    [_screen consume:data];
}

- (BOOL)isPresentationEnabled { return _screen.presentationEnabled; }
- (void)setPresentationEnabled:(BOOL)enabled {
    _screen.presentationEnabled = enabled;
    if (!enabled) { [self resignFirstResponder]; _inputField.text = @""; }
}

- (void)setInputHandler:(void (^)(NSData *data))inputHandler {
    _inputHandler = [inputHandler copy];
    _controls.userInteractionEnabled = _inputHandler != nil;
    _controls.alpha = _inputHandler ? 1 : 0.5;
    if (!_inputHandler) { [self resignFirstResponder]; _inputField.text = @""; }
}

- (void)setStatusText:(NSString *)statusText {
    if ([_statusText isEqualToString:statusText]) return;
    _statusText = [statusText copy];
    _status.text = statusText;
    _status.hidden = !statusText.length;
    [self setNeedsLayout];
}

- (void)setResizeHandler:(void (^)(unsigned short rows, unsigned short columns))resizeHandler {
    _resizeHandler = [resizeHandler copy];
    // A new launchd terminal can attach after this view was laid out. Publish the existing geometry without needing a size change.
    if (_resizeHandler) _resizeHandler((unsigned short)_screen.rows, (unsigned short)_screen.columns);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat statusHeight = self.statusText.length ? ceil([_status sizeThatFits:CGSizeMake(MAX(0, self.bounds.size.width - 16), CGFLOAT_MAX)].height) + 16 : 0;
    statusHeight = MIN(statusHeight, self.bounds.size.height);
    _status.frame = CGRectMake(8, 8, MAX(0, self.bounds.size.width - 16), MAX(0, statusHeight - 16));
    CGFloat controlsHeight = MIN(44, MAX(0, self.bounds.size.height - statusHeight));
    CGFloat oldWidth = _controls.bounds.size.width;
    NSInteger page = oldWidth > 0 ? (NSInteger)llround(_controls.contentOffset.x / oldWidth) : 0;
    _controls.frame = CGRectMake(0, self.bounds.size.height - controlsHeight, self.bounds.size.width, controlsHeight);
    if (oldWidth != _controls.bounds.size.width) {
        [_controls layoutIfNeeded];
        _controls.contentOffset = CGPointMake(MAX(0, MIN(page, 1)) * _controls.bounds.size.width, 0);
    }
    _display.frame = CGRectMake(0, statusHeight, self.bounds.size.width, MAX(0, self.bounds.size.height - statusHeight - controlsHeight));
    NSUInteger columns = MAX(1, (NSInteger)((self.bounds.size.width - 16) / _cellWidth));
    NSUInteger rows = MAX(1, (NSInteger)((_display.bounds.size.height - 16) / _cellHeight));
    columns = MIN(columns, 400); rows = MIN(rows, 200);
    if (rows != _screen.rows || columns != _screen.columns) {
        [_screen resizeRows:rows columns:columns];
        if (self.resizeHandler) self.resizeHandler((unsigned short)rows, (unsigned short)columns);
    }
}
@end
#endif
