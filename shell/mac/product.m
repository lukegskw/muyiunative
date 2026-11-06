// Phase 5's double-click path: game.plist identifies a bundled, read-only disc set and its persistent cards; the generic app opens previously built game apps.
// Foundation locates Application Support (File System Programming Guide, Accessing Files and Directories); no path depends on Finder's working directory.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <string.h>
#include "product.h"
#include "ps1.h"
#include "report.h"
#include "build.h"

static BOOL copy_string(char *out, size_t cap, id value) {
    if (![value isKindOfClass:NSString.class]) return NO;
    const char *s = [value UTF8String]; if (!s || strlen(s) >= cap) return NO;
    memcpy(out, s, strlen(s) + 1); return YES;
}
int front_product_load(front_product *p) {
    const char *resources = NSBundle.mainBundle.resourceURL.path.fileSystemRepresentation;
    return resources ? front_product_read(p, resources) : 0;
}
int front_save_directory(char *out, size_t cap, const char *id) {
    NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *url = [[base URLByAppendingPathComponent:@"muyiunative/Saves" isDirectory:YES] URLByAppendingPathComponent:@(id) isDirectory:YES];
    if (!url || !copy_string(out, cap, url.path)) { RED("host: cannot locate Application Support for memory cards"); return -1; }
    return 0;
}
void front_error(const char *message) {
    NSApplication.sharedApplication.activationPolicy = NSApplicationActivationPolicyRegular;
    [NSApp activate]; NSAlert *a = [NSAlert new]; a.messageText = @"muyiunative"; a.informativeText = @(message); [a runModal];
}

@interface Launcher : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@property NSPopUpButton *recent;
@property NSUserDefaults *prefs;
- (void)open:(id)sender;
@end
@implementation Launcher
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return YES; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app { (void)app; return front_build_terminate() ? NSTerminateLater : NSTerminateNow; }
- (void)build:(id)sender { (void)sender; front_build_game_app(); }
- (void)launch:(NSURL *)url {
    NSBundle *bundle = [NSBundle bundleWithURL:url];
    if (![bundle.bundleIdentifier hasPrefix:@"org.muyiunative.game."]) { front_error("Choose a game app created by muyiunative build."); return; }
    [NSWorkspace.sharedWorkspace openApplicationAtURL:url configuration:NSWorkspaceOpenConfiguration.configuration completionHandler:^(NSRunningApplication *app, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error) { front_error(error.localizedDescription.UTF8String); return; }
            (void)app; NSMutableArray *paths = [[self.prefs stringArrayForKey:@"RecentGames"] mutableCopy] ?: [NSMutableArray new];
            [paths removeObject:url.path]; [paths insertObject:url.path atIndex:0]; if (paths.count > 12) [paths removeLastObject];
            [self.prefs setObject:paths forKey:@"RecentGames"]; [NSApp terminate:nil];
        });
    }];
}
- (void)open:(id)sender {
    (void)sender; NSOpenPanel *p = NSOpenPanel.openPanel; p.allowedContentTypes = @[UTTypeApplicationBundle]; p.canChooseDirectories = NO;
    p.message = @"Choose a game app created by muyiunative build.";
    if ([p runModal] == NSModalResponseOK) [self launch:p.URL];
}
- (void)play:(id)sender { (void)sender; if (self.recent.selectedItem.representedObject) [self launch:[NSURL fileURLWithPath:self.recent.selectedItem.representedObject]]; }
@end

void front_launcher(void) {
    NSApplication *app = NSApplication.sharedApplication; app.activationPolicy = NSApplicationActivationPolicyRegular;
    Launcher *l = [Launcher new]; l.prefs = [[NSUserDefaults alloc] initWithSuiteName:@"org.muyiunative"]; app.delegate = l;
    NSMenu *bar = [NSMenu new], *menu = [NSMenu new]; NSMenuItem *item = [NSMenuItem new];
    front_reports_menu((__bridge void *)menu);
    [menu addItemWithTitle:@"Build Game App…" action:@selector(build:) keyEquivalent:@"n"].target = l;
    [menu addItemWithTitle:@"Open Game App…" action:@selector(open:) keyEquivalent:@"o"].target = l;
    [menu addItemWithTitle:@"Quit muyiunative" action:@selector(terminate:) keyEquivalent:@"q"]; item.submenu = menu; [bar addItem:item]; app.mainMenu = bar;
    l.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 310) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    l.window.title = @"muyiunative"; l.window.releasedWhenClosed = NO;
    NSTextField *title = [NSTextField labelWithString:@"Your PlayStation games on the Mac"]; title.font = [NSFont boldSystemFontOfSize:21]; title.frame = NSMakeRect(24, 252, 475, 32); [l.window.contentView addSubview:title];
    NSTextField *note = [NSTextField wrappingLabelWithString:@"Build a native app from a CUE or BIN dump of your disc. Apple's free Command Line Tools are needed only when building."]; note.frame = NSMakeRect(24, 185, 472, 58); [l.window.contentView addSubview:note];
    NSButton *build = [NSButton buttonWithTitle:@"Build Game App…" target:l action:@selector(build:)]; build.frame = NSMakeRect(24, 142, 190, 32); [l.window.contentView addSubview:build];
    l.recent = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(24, 88, 470, 30) pullsDown:NO];
    for (NSString *path in [l.prefs stringArrayForKey:@"RecentGames"]) if ([NSFileManager.defaultManager fileExistsAtPath:path]) {
        [l.recent addItemWithTitle:path.lastPathComponent.stringByDeletingPathExtension]; l.recent.lastItem.representedObject = path;
    }
    l.recent.enabled = l.recent.numberOfItems > 0; if (!l.recent.enabled) [l.recent addItemWithTitle:@"No recent games"];
    [l.window.contentView addSubview:l.recent];
    NSButton *open = [NSButton buttonWithTitle:@"Open Game App…" target:l action:@selector(open:)]; open.frame = NSMakeRect(24, 28, 190, 32); [l.window.contentView addSubview:open];
    NSButton *play = [NSButton buttonWithTitle:@"Play Recent" target:l action:@selector(play:)]; play.frame = NSMakeRect(320, 28, 174, 32); play.enabled = l.recent.enabled; [l.window.contentView addSubview:play];
    [l.window center]; [l.window makeKeyAndOrderFront:nil]; [app activate]; [app run];
}
