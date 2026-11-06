// Phase 7's terminal-free build UI. The signed BuildKit is copied by tools/player_build.py to a disposable cache; CLT Python, make and clang are child processes.
// One background reader owns the output pipe and queues complete UTF-8 lines before completion. AppKit and all build/window state stay on the main thread.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdio.h>
#include <string.h>
#include "build.h"
#include "product.h"
#include "report.h"

static NSString *tool_output(NSString *exe, NSArray<NSString *> *arguments) {
    NSTask *task = [NSTask new]; NSPipe *pipe = NSPipe.pipe;
    task.executableURL = [NSURL fileURLWithPath:exe]; task.arguments = arguments; task.standardOutput = pipe; task.standardError = NSFileHandle.fileHandleWithNullDevice;
    NSError *error; if (![task launchAndReturnError:&error]) return nil;
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile]; [task waitUntilExit];
    if (task.terminationStatus) return nil;
    return [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

@interface GameBuild : NSWindowController <NSWindowDelegate>
@property NSTextField *status;
@property NSProgressIndicator *progress;
@property NSTextView *log;
@property NSButton *cancelButton, *showButton, *openButton;
@property NSTask *task;
@property NSURL *output;
@property NSMutableData *pending;
@property BOOL cancelled, quitWhenDone, closeWhenDone, preparing;
- (void)choose;
@end

@implementation GameBuild
- (instancetype)init {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 510) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:w])) return nil;
    w.title = @"Build Game App"; w.releasedWhenClosed = NO; w.delegate = self;
    self.status = [NSTextField labelWithString:@"Choose your game discs to build one native app."]; self.status.frame = NSMakeRect(20, 460, 720, 30); [w.contentView addSubview:self.status];
    self.progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 434, 720, 18)]; self.progress.style = NSProgressIndicatorStyleBar; self.progress.indeterminate = YES; [w.contentView addSubview:self.progress];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 70, 720, 350)]; scroll.hasVerticalScroller = YES; scroll.borderType = NSBezelBorder;
    self.log = [[NSTextView alloc] initWithFrame:scroll.bounds]; self.log.editable = NO; self.log.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.log.autoresizingMask = NSViewWidthSizable; self.log.textContainer.widthTracksTextView = YES; scroll.documentView = self.log; [w.contentView addSubview:scroll];
    self.cancelButton = [NSButton buttonWithTitle:@"Cancel Build" target:self action:@selector(cancel:)]; self.cancelButton.frame = NSMakeRect(20, 20, 150, 32); self.cancelButton.enabled = NO; [w.contentView addSubview:self.cancelButton];
    self.showButton = [NSButton buttonWithTitle:@"Show App in Finder" target:self action:@selector(show:)]; self.showButton.frame = NSMakeRect(380, 20, 180, 32); self.showButton.enabled = NO; [w.contentView addSubview:self.showButton];
    self.openButton = [NSButton buttonWithTitle:@"Open Game App" target:self action:@selector(open:)]; self.openButton.frame = NSMakeRect(570, 20, 170, 32); self.openButton.enabled = NO; [w.contentView addSubview:self.openButton];
    [w center]; return self;
}
- (void)appendLine:(NSData *)data {
    // The headless child retains its normal stdout; only the Finder build window/reporting applies the session's guest-text choice.
    if (data.length >= 5 && !memcmp(data.bytes, "tty: ", 5) && !front_reports_guest_output()) return;
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
    text = [text stringByReplacingOccurrencesOfString:NSHomeDirectory() withString:@"~"];
    // The visual check showed black default attributed text on a dark background. Give appended lines the view's font and the system's adaptive text color.
    [self.log.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:@{NSFontAttributeName: self.log.font, NSForegroundColorAttributeName: NSColor.textColor}]];
    [self.log scrollRangeToVisible:NSMakeRange(self.log.string.length, 0)]; fputs(text.UTF8String, stderr);
    // Keep the current step readable for a player; the detailed output remains available in the log below.
    NSDictionary *steps = @{@"build: preparing the launcher build kit": @"Preparing build files…", @"build: building the capture host": @"Preparing build tools…",
        @"build: capturing interpreted game code": @"Analyzing your game…", @"build: generating native code": @"Preparing native game code…",
        @"build: compiling native game code": @"Compiling your game…", @"build: checking the route against the interpreter": @"Checking the compiled game…",
        @"build: packing and signing the game app": @"Creating your game app…"};
    NSString *step = steps[[text stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet]];
    if (step && !self.cancelled) self.status.stringValue = step;
}
- (void)append:(NSData *)data final:(BOOL)final {
    [self.pending appendData:data];
    for (;;) {
        const char *bytes = self.pending.bytes, *newline = self.pending.length ? memchr(bytes, '\n', self.pending.length) : NULL;
        NSUInteger n = newline ? (NSUInteger)(newline - bytes + 1) : final ? self.pending.length : 0;
        if (!n) break;
        [self appendLine:[self.pending subdataWithRange:NSMakeRange(0, n)]]; [self.pending replaceBytesInRange:NSMakeRange(0, n) withBytes:NULL length:0];
    }
}
- (void)run:(NSArray<NSURL *> *)discs python:(NSString *)python {
    NSURL *disc = discs.firstObject;
    NSURL *kit = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"BuildKit" isDirectory:YES];
    NSURL *entry = [kit URLByAppendingPathComponent:@"tools/player_build.py"];
    if (![NSFileManager.defaultManager isReadableFileAtPath:entry.path]) { self.preparing = NO; front_error("This launcher has no build kit. Build it again with make -C shell/mac app."); return; }
    NSSavePanel *save = NSSavePanel.savePanel; save.allowedContentTypes = @[UTTypeApplicationBundle]; save.canCreateDirectories = YES; save.extensionHidden = NO;
    NSURL *apps = [[NSFileManager.defaultManager URLsForDirectory:NSApplicationDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"muyiunative" isDirectory:YES];
    NSError *error;
    if (!apps || ![NSFileManager.defaultManager createDirectoryAtURL:apps withIntermediateDirectories:YES attributes:nil error:&error]) { self.preparing = NO; front_error("Cannot create your Applications/muyiunative folder."); return; }
    save.directoryURL = apps; save.nameFieldStringValue = [disc.lastPathComponent.stringByDeletingPathExtension stringByAppendingPathExtension:@"app"];
    save.message = @"Choose where to save the native game app.";
    [save beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        if (result == NSModalResponseOK) [self startDiscs:discs output:save.URL python:python]; else self.preparing = NO;
    }];
}
- (void)startDiscs:(NSArray<NSURL *> *)discs output:(NSURL *)output python:(NSString *)python {
    self.preparing = NO;
    NSURL *entry = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"BuildKit/tools/player_build.py"];
    NSURL *cache = [[NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"muyiunative/Builds" isDirectory:YES];
    if (!cache) { front_error("Cannot locate the temporary build cache."); return; }
    NSError *error;
    self.output = output; self.pending = [NSMutableData new]; self.cancelled = NO; self.quitWhenDone = NO; self.closeWhenDone = NO;
    self.log.string = @""; self.showButton.enabled = self.openButton.enabled = NO; self.cancelButton.enabled = YES;
    self.status.stringValue = @"Preparing the build…"; self.progress.indeterminate = YES; [self.progress startAnimation:nil]; [self showWindow:nil]; [self.window makeKeyAndOrderFront:nil];
    NSTask *task = [NSTask new]; NSPipe *pipe = NSPipe.pipe; task.executableURL = [NSURL fileURLWithPath:python];
    NSMutableArray *arguments = [@[@"-B", entry.path, discs.firstObject.path, @"--output", self.output.path, @"--work-root", cache.path] mutableCopy];
    for (NSURL *disc in [discs subarrayWithRange:NSMakeRange(1, discs.count - 1)]) [arguments addObjectsFromArray:@[@"--disc", disc.path]];
    task.arguments = arguments;
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy]; [env removeObjectForKey:@"PYTHONPATH"]; [env removeObjectForKey:@"PYTHONHOME"];
    env[@"PYTHONDONTWRITEBYTECODE"] = @"1"; env[@"PYTHONNOUSERSITE"] = @"1"; task.environment = env;
    task.standardOutput = pipe; task.standardError = pipe; self.task = task;
    if (![task launchAndReturnError:&error]) { self.task = nil; self.cancelButton.enabled = NO; [self.progress stopAnimation:nil]; self.status.stringValue = @"Build could not start."; front_error(error.localizedDescription.UTF8String); return; }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (;;) { NSData *data = [pipe.fileHandleForReading availableData]; if (!data.length) break; dispatch_async(dispatch_get_main_queue(), ^{ [self append:data final:NO]; }); }
        [task waitUntilExit];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self append:NSData.data final:YES]; self.task = nil; self.cancelButton.enabled = NO; [self.progress stopAnimation:nil];
            BOOL success = !self.cancelled && task.terminationStatus == 0;
            self.progress.indeterminate = NO; self.progress.maxValue = 1; self.progress.doubleValue = success ? 1 : 0;
            self.status.stringValue = self.cancelled && task.terminationStatus == 130 ? @"Build cancelled. Temporary files removed." : success ? @"Your game app is ready." : @"Build stopped. See the log above.";
            self.showButton.enabled = self.openButton.enabled = success;
            if (success) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[self.output]];
            if (self.quitWhenDone) [NSApp replyToApplicationShouldTerminate:YES]; else if (self.closeWhenDone) [self close];
        });
    });
}
- (void)choose {
    if (self.task) { [self showWindow:nil]; return; } if (self.preparing) return; self.preparing = YES;
    // The four-disc UI check returned from Open with no order dialog: NSApp.mainWindow can be nil during that transition. Own a visible, stable sheet parent.
    [self showWindow:nil]; [self.window makeKeyAndOrderFront:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *developer = tool_output(@"/usr/bin/xcode-select", @[@"-p"]);
        NSString *python = developer ? tool_output(@"/usr/bin/xcrun", @[@"--find", @"python3"]) : nil;
        BOOL ready = python.length && tool_output(@"/usr/bin/xcrun", @[@"--find", @"clang"]).length && tool_output(@"/usr/bin/xcrun", @[@"--show-sdk-path"]).length;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!ready) {
                NSAlert *a = [NSAlert new]; a.messageText = @"Install Apple Command Line Tools"; a.informativeText = @"Building a game app needs Apple's free Command Line Tools. Install them, then choose Build Game App again.";
                [a addButtonWithTitle:@"Install Tools"]; [a addButtonWithTitle:@"Cancel"];
                [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
                    self.preparing = NO;
                    if (result == NSAlertFirstButtonReturn) { NSTask *install = [NSTask new]; install.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcode-select"]; install.arguments = @[@"--install"]; NSError *error; if (![install launchAndReturnError:&error]) front_error(error.localizedDescription.UTF8String); }
                }];
                return;
            }
            NSOpenPanel *panel = NSOpenPanel.openPanel; panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"cue"] ?: UTTypeData, [UTType typeWithFilenameExtension:@"bin"] ?: UTTypeData,
                [UTType typeWithFilenameExtension:@"m3u"] ?: UTTypeData, [UTType typeWithFilenameExtension:@"m3u8"] ?: UTTypeData];
            panel.canChooseDirectories = NO; panel.allowsMultipleSelection = YES;
            // AX calls timed out while runModal held this dispatched main-queue block. Asynchronous sheets leave that queue available to AppKit and accessibility.
            panel.message = @"Choose your CUE/BIN discs together, or one M3U playlist in disc order. One game app will include the entire set.";
            [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
                if (result != NSModalResponseOK) { self.preparing = NO; return; }
                NSArray<NSURL *> *discs = [panel.URLs sortedArrayUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) { return [a.path localizedStandardCompare:b.path]; }];
                if (discs.count == 1) { [self run:discs python:python]; return; }
                NSMutableString *order = [NSMutableString stringWithString:@"One app will contain these discs. The first disc determines the app's memory cards.\n\n"];
                for (NSUInteger i = 0; i < discs.count; i++) [order appendFormat:@"%lu. %@\n", i + 1, discs[i].lastPathComponent];
                [order appendString:@"\nFor a different order, choose an M3U playlist instead."];
                NSAlert *confirm = [NSAlert new]; confirm.messageText = @"Disc order"; confirm.informativeText = order;
                [confirm addButtonWithTitle:@"Build One App"]; [confirm addButtonWithTitle:@"Cancel"];
                [confirm beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
                    if (response == NSAlertFirstButtonReturn) [self run:discs python:python]; else self.preparing = NO;
                }];
            }];
        });
    });
}
- (void)cancel:(id)sender { (void)sender; if (!self.task || self.cancelled) return; self.cancelled = YES; self.cancelButton.enabled = NO; self.status.stringValue = @"Cancelling build…"; [self.task terminate]; }
- (void)show:(id)sender { (void)sender; if (self.output) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[self.output]]; }
- (void)open:(id)sender { (void)sender; if (self.output && ![NSWorkspace.sharedWorkspace openURL:self.output]) front_error("Cannot open the completed game app."); }
- (BOOL)windowShouldClose:(NSWindow *)window {
    (void)window; if (!self.task) return YES;
    NSAlert *a = [NSAlert new]; a.messageText = @"Cancel this build?"; a.informativeText = @"The temporary build files will be removed."; [a addButtonWithTitle:@"Cancel Build"]; [a addButtonWithTitle:@"Keep Building"];
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        if (result == NSAlertFirstButtonReturn) { self.closeWhenDone = YES; if (self.task) [self cancel:nil]; else [self close]; }
    }]; return NO;
}
@end

static GameBuild *g_build;
void front_build_game_app(void) { if (!g_build) g_build = [GameBuild new]; [g_build choose]; }
int front_build_terminate(void) {
    if (!g_build.task) return 0;
    NSAlert *a = [NSAlert new]; a.messageText = @"Cancel the build and quit?"; a.informativeText = @"The build will stop and its temporary files will be removed before quitting.";
    [a addButtonWithTitle:@"Cancel Build and Quit"]; [a addButtonWithTitle:@"Keep Building"];
    [a beginSheetModalForWindow:g_build.window completionHandler:^(NSModalResponse result) {
        if (result != NSAlertFirstButtonReturn) { [NSApp replyToApplicationShouldTerminate:NO]; return; }
        g_build.quitWhenDone = YES; if (g_build.task) [g_build cancel:nil]; else [NSApp replyToApplicationShouldTerminate:YES];
    }]; return 1;
}
