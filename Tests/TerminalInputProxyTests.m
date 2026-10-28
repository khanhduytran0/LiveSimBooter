// Runs real local UIKit through Mac Catalyst. This checks input API routing, not device keyboard focus or gestures.
#import "../Host/TerminalView.m"
#include <assert.h>
#include <stdio.h>

static LSBTerminalInputField *findField(UIView *view) {
    if ([view isKindOfClass:LSBTerminalInputField.class]) return (LSBTerminalInputField *)view;
    for (UIView *child in view.subviews) {
        LSBTerminalInputField *field = findField(child);
        if (field) return field;
    }
    return nil;
}

int main(void) {
    @autoreleasepool {
        LSBTerminalView *terminal = [[LSBTerminalView alloc] initWithFrame:CGRectMake(0, 0, 320, 480)];
        NSMutableData *received = [NSMutableData new];
        terminal.inputHandler = ^(NSData *data) { [received appendData:data]; };
        terminal.presentationEnabled = YES;
        LSBTerminalInputField *field = findField(terminal);
        assert(field && [field conformsToProtocol:@protocol(UITextInput)] && !field.hidden && field.alpha == 1);

        [field insertText:@"a\n"];
        [field deleteBackward];
        assert([received isEqualToData:[@"a\r\177" dataUsingEncoding:NSUTF8StringEncoding]]);
        UITextRange *range = [field textRangeFromPosition:field.beginningOfDocument toPosition:field.endOfDocument];
        assert(range);
        [field replaceRange:range withText:@"b"];
        assert([received isEqualToData:[@"a\r\177b" dataUsingEncoding:NSUTF8StringEncoding]] && !field.text.length);
        [field setMarkedText:@"x" selectedRange:NSMakeRange(1, 0)];
        assert(received.length == 4 && field.markedTextRange);
        [field unmarkText];
        assert([received isEqualToData:[@"a\r\177bx" dataUsingEncoding:NSUTF8StringEncoding]] && !field.text.length);

        [field hardwareKey:[UIKeyCommand keyCommandWithInput:UIKeyInputUpArrow modifierFlags:0 action:@selector(hardwareKey:)]];
        [field hardwareKey:[UIKeyCommand keyCommandWithInput:@"c" modifierFlags:UIKeyModifierControl action:@selector(hardwareKey:)]];
        [field textFieldShouldReturn:field];
        assert([received isEqualToData:[@"a\r\177bx\033[A\003\r" dataUsingEncoding:NSUTF8StringEncoding]]);
        [terminal appendOutput:[@"\033[?2004h" dataUsingEncoding:NSUTF8StringEncoding]];
        NSData *expectedPaste = [@"a\r\177bx\033[A\003\r\033[200~p\nq\033[201~" dataUsingEncoding:NSUTF8StringEncoding];
        [field pasteItemProviders:@[[[NSItemProvider alloc] initWithObject:@"p\nq"]]];
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
        while (received.length < expectedPaste.length && deadline.timeIntervalSinceNow > 0)
            [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        assert([received isEqualToData:expectedPaste]);

        NSUInteger count = received.length;
        terminal.presentationEnabled = NO;
        [field insertText:@"ignored"];
        [field deleteBackward];
        [field replaceRange:[field textRangeFromPosition:field.beginningOfDocument toPosition:field.endOfDocument] withText:@"ignored"];
        assert(received.length == count && !field.text.length);
        terminal.inputHandler = nil;
        terminal.presentationEnabled = YES;
        [field insertText:@"closed"];
        [field deleteBackward];
        assert(received.length == count && !field.text.length);
        puts("TerminalInputProxy: real UITextInput keys, replacement, marked commit, hardware keys, paste and inactive input PASS");
    }
    return 0;
}
