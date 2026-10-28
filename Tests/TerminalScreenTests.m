#define LSB_TERMINAL_SCREEN_TESTING
#import "../Host/TerminalView.m"
#include <assert.h>
#include <stdio.h>

static void feed(LSBTerminalScreen *screen, NSString *text) { [screen consume:[text dataUsingEncoding:NSUTF8StringEncoding]]; }

static LSBTerminalScreen *screen(NSUInteger rows, NSUInteger columns) {
    LSBTerminalScreen *screen = [LSBTerminalScreen new];
    [screen resizeRows:rows columns:columns];
    return screen;
}

static void drainMainQueue(void) {
    __block BOOL drained = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ drained = YES; });
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!drained && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    assert(drained);
}

static void presentationTests(void) {
    LSBTerminalScreen *terminal = screen(4, 30);
    __weak LSBTerminalScreen *weakTerminal = terminal;
    __block unsigned renders = 0, replies = 0;
    __block NSString *painted;
    terminal.presentationHandler = ^{ ++renders; painted = [weakTerminal text]; };
    terminal.reply = ^(NSData *data) {
        ++replies;
        assert([data isEqualToData:[@"\033[1;8R" dataUsingEncoding:NSUTF8StringEncoding]]);
        feed(weakTerminal, @"!"); // A synchronous echo must still parse while presentation is hidden.
    };

    // Startup output precedes first appearance. A split Unicode/ANSI stream still replies immediately and paints nothing.
    NSData *startup = [@"start é\033[6n" dataUsingEncoding:NSUTF8StringEncoding];
    for (NSUInteger i = 0; i < startup.length; ++i) [terminal consume:[startup subdataWithRange:NSMakeRange(i, 1)]];
    assert(replies == 1 && !renders && [[terminal text] isEqualToString:@"start é! "]);
    drainMainQueue();
    assert(!renders);
    terminal.presentationEnabled = YES;
    assert(!renders);
    drainMainQueue();
    assert(renders == 1 && [painted isEqualToString:@"start é! "]);

    // A burst has the same final screen as immediate parsing, with one render despite many newline fragments.
    for (unsigned i = 0; i < 200; ++i) { feed(terminal, @"row"); feed(terminal, @"\r\n"); }
    assert(renders == 1);
    NSString *burst = [terminal text];
    drainMainQueue();
    assert(renders == 2 && [painted isEqualToString:burst]);

    // Hide before an already queued redraw runs, then resize and reach EOF without another output callback.
    feed(terminal, @"final output");
    terminal.presentationEnabled = NO;
    [terminal resizeRows:3 columns:25];
    terminal.reply = nil;
    drainMainQueue();
    assert(renders == 2);
    NSString *final = [terminal text];
    terminal.presentationEnabled = YES;
    drainMainQueue();
    assert(renders == 3 && [painted isEqualToString:final]);
    terminal.presentationEnabled = NO;
    terminal.presentationEnabled = YES;
    drainMainQueue();
    assert(renders == 3); // Showing an unchanged screen needs no replacement of attributedText.

    // Reentrant output during presentation invalidates the following pass instead of losing that change.
    __block BOOL inject = YES;
    terminal.presentationHandler = ^{
        ++renders;
        painted = [weakTerminal text];
        if (inject) { inject = NO; feed(weakTerminal, @"!"); }
    };
    feed(terminal, @"x");
    drainMainQueue();
    drainMainQueue();
    assert(renders == 5 && [painted isEqualToString:[terminal text]] && [painted hasSuffix:@"x! "]);
    terminal.presentationHandler = nil;
}

int main(void) {
    @autoreleasepool {
        presentationTests();
        LSBTerminalScreen *editing = screen(3, 10);
        feed(editing, @"abcdef\rXY\033[K");
        assert([[editing text] isEqualToString:@"XY "] && editing.column == 2);
        feed(editing, @"\bZ\033[2DX");
        assert([[editing text] isEqualToString:@"XZ"] && editing.column == 1);
        feed(editing, @"\033[2;4H!\033[6n");
        assert(editing.row == 1 && editing.column == 4);
        feed(editing, @"\033[2J\033[H");
        assert([[editing text] isEqualToString:@" "] && !editing.row && !editing.column);

        LSBTerminalScreen *wrapping = screen(3, 4);
        feed(wrapping, @"ABCD\033[31mE\033[0m");
        assert([[wrapping text] isEqualToString:@"ABCD\nE "] && wrapping.row == 1 && wrapping.column == 1);
        feed(wrapping, @"\r\nF\r\nG");
        assert([[wrapping text] isEqualToString:@"ABCD\nE\nF\nG "]);

        LSBTerminalScreen *whole = screen(4, 20), *split = screen(4, 20);
        NSString *sequence = @"hello é 🙂 e\u0301\033[2;2Hxyz\033]0;ignored title\033\\!\033Pignored query\033\\\033[2D!";
        NSData *encoded = [sequence dataUsingEncoding:NSUTF8StringEncoding];
        [whole consume:encoded];
        for (NSUInteger index = 0; index < encoded.length; ++index) [split consume:[encoded subdataWithRange:NSMakeRange(index, 1)]];
        assert([[whole text] isEqualToString:[split text]] && whole.row == split.row && whole.column == split.column);
        assert([[split text] containsString:@"é 🙂 e\u0301"] && ![[split text] containsString:@"ignored"]);
        const uint8_t invalid[] = {0xc0, 0xe2, 'a'};
        [split consume:[NSData dataWithBytes:invalid length:sizeof(invalid)]];
        assert([[split text] containsString:@"��a"]);

        LSBTerminalScreen *replies = screen(3, 10);
        NSMutableData *received = [NSMutableData new];
        replies.reply = ^(NSData *data) { [received appendData:data]; };
        feed(replies, @"abc\033[6n\033[c\033[>c\033[5n");
        NSString *response = [[NSString alloc] initWithData:received encoding:NSUTF8StringEncoding];
        assert([response isEqualToString:@"\033[1;4R\033[?1;2c\033[>0;1;0c\033[0n"]);
        feed(replies, @"\033[?25l"); assert(!replies.cursorVisible);
        feed(replies, @"\033[?25h"); assert(replies.cursorVisible);

        LSBTerminalScreen *paste = screen(3, 20);
        assert([[paste pasteSequenceForText:@"é\nnext"] isEqualToString:@"é\rnext"]);
        feed(paste, @"\033[?200"); feed(paste, @"4h"); // Mode changes may arrive split across output reads.
        assert(paste.bracketedPasteEnabled);
        assert([[paste pasteSequenceForText:@"é\nnext"] isEqualToString:@"\033[200~é\nnext\033[201~"]);
        assert(![paste pasteSequenceForText:@""].length);
        feed(paste, @"\033[?25;2004l");
        assert(!paste.cursorVisible && !paste.bracketedPasteEnabled);
        feed(paste, @"\033[?25;2004h");
        assert(paste.cursorVisible && paste.bracketedPasteEnabled);
        feed(paste, @"\033c");
        assert(!paste.bracketedPasteEnabled && paste.cursorVisible);

        LSBTerminalScreen *controlEcho = screen(3, 40);
        __weak LSBTerminalScreen *weakControlEcho = controlEcho;
        controlEcho.reply = ^(NSData *data) {
            NSString *response = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            feed(weakControlEcho, [response stringByReplacingOccurrencesOfString:@"\033" withString:@"^["]);
        };
        feed(controlEcho, @"abc\033[6n");
        assert([[controlEcho text] isEqualToString:@"abc^[[1;4R "]);

        LSBTerminalScreen *rawEcho = screen(3, 40);
        __weak LSBTerminalScreen *weakRawEcho = rawEcho;
        __block unsigned echoedReplies = 0;
        rawEcho.reply = ^(NSData *data) {
            assert(++echoedReplies <= 6); // A DA response must not recursively produce another response.
            [weakRawEcho consume:data];
        };
        feed(rawEcho, @"abc\033[6n\033[c\033[>c\033[0c\033[>0c\033[5n!");
        assert(echoedReplies == 6 && [[rawEcho text] isEqualToString:@"abc! "]);

        LSBTerminalScreen *partialReply = screen(3, 10);
        __weak LSBTerminalScreen *weakPartialReply = partialReply;
        partialReply.reply = ^(NSData *data) { (void)data; feed(weakPartialReply, @"\033[2;"); };
        feed(partialReply, @"\033[6n3HX");
        assert(partialReply.row == 1 && partialReply.column == 3 && [[partialReply text] isEqualToString:@"\n  X "]);

        LSBTerminalScreen *characters = screen(3, 10);
        feed(characters, @"abc\r\033[P\033[@");
        assert([[characters text] isEqualToString:@" bc"]);
        feed(characters, @"\033[2C\0337\033[3;9H!\0338X");
        assert(characters.row == 0 && characters.column == 3 && [[characters text] hasPrefix:@" bX"]);
        feed(characters, @"\033[2K");
        assert([[characters text] hasPrefix:@"    \n"]);

        LSBTerminalScreen *bounded = screen(3, 10);
        for (unsigned index = 0; index < 1050; ++index) feed(bounded, @"line\r\n");
        assert([[bounded text] componentsSeparatedByString:@"\n"].count <= 1003);
        [bounded resizeRows:2 columns:5];
        assert(bounded.row < 2 && bounded.column < 5);
        feed(bounded, @"\033[3J\033[2J\033[H");
        assert([[bounded text] isEqualToString:@" "]);
    }
    puts("TerminalScreen: presentation, split UTF-8/ANSI, editing, bracketed paste, reentrant replies, and bounded resize passed");
    return 0;
}
