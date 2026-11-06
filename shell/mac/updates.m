// Sparkle owns downloading, signature checks, installation and relaunch (https://sparkle-project.org/documentation/programmatic-setup/).
// Load it only in the packaged launcher; developer/capture hosts and generated game apps have no framework dependency or update traffic.
#import <Cocoa/Cocoa.h>
#include "updates.h"

// Public Objective-C entry point, loaded from the sealed bundle instead of linking Sparkle into every generated game/capture host.
@protocol FrontUpdater
@property BOOL automaticallyChecksForUpdates;
@end
@protocol FrontUpdaterController
@property (readonly) id<FrontUpdater> updater;
- (instancetype)initWithStartingUpdater:(BOOL)start updaterDelegate:(id)delegate userDriverDelegate:(id)driver;
- (void)checkForUpdates:(id)sender;
@end

@interface UpdateMenu : NSObject
@property id<FrontUpdaterController> controller;
@property (weak) NSWindow *window;
@property NSButton *reminder;
@end
@implementation UpdateMenu
- (void)automatic:(NSMenuItem *)sender { (void)sender; self.controller.updater.automaticallyChecksForUpdates = !self.controller.updater.automaticallyChecksForUpdates; }
- (BOOL)validateMenuItem:(NSMenuItem *)item { item.state = self.controller.updater.automaticallyChecksForUpdates ? NSControlStateValueOn : NSControlStateValueOff; return YES; }
// A signed background check succeeded while the launcher was inactive, with no focused alert. Keep availability visible beside Build Game App; Sparkle retains
// its standard user driver and installer. didFindValidUpdate is emitted only after validating the feed and selecting a compatible newer version.
- (void)updater:(id)updater didFindValidUpdate:(id)update {
    (void)updater; (void)update;
    if (self.reminder) return;
    self.reminder = [NSButton buttonWithTitle:@"Update Available…" target:self.controller action:@selector(checkForUpdates:)]; self.reminder.frame = NSMakeRect(320, 142, 174, 32);
    [self.window.contentView addSubview:self.reminder];
}
- (void)updater:(id)updater didFinishUpdateCycleForUpdateCheck:(NSInteger)check error:(NSError *)error {
    (void)updater; (void)check; (void)error; [self.reminder removeFromSuperview]; self.reminder = nil;
}
@end

static UpdateMenu *g_updates;
void front_updates_window(void *window) { g_updates.window = (__bridge NSWindow *)window; }
void front_updates_menu(void *menu) {
    NSMenu *m = (__bridge NSMenu *)menu;
    NSURL *url = [NSBundle.mainBundle.privateFrameworksURL URLByAppendingPathComponent:@"Sparkle.framework"];
    if (![NSFileManager.defaultManager fileExistsAtPath:url.path]) return;
    NSError *error; NSBundle *framework = [NSBundle bundleWithURL:url];
    if (![framework loadAndReturnError:&error]) { fprintf(stderr, "[RED] updates: cannot load Sparkle: %s\n", error.localizedDescription.UTF8String); return; }
    Class controllerClass = NSClassFromString(@"SPUStandardUpdaterController");
    static id<FrontUpdaterController> controller;
    UpdateMenu *actions = [UpdateMenu new]; g_updates = actions;
    controller = [(id<FrontUpdaterController>)[controllerClass alloc] initWithStartingUpdater:YES updaterDelegate:actions userDriverDelegate:nil];
    if (!controller) { fprintf(stderr, "[RED] updates: cannot start Sparkle\n"); return; }
    [m addItemWithTitle:@"Check for Updates…" action:@selector(checkForUpdates:) keyEquivalent:@""].target = controller;
    actions.controller = controller;
    [m addItemWithTitle:@"Automatically Check for Updates" action:@selector(automatic:) keyEquivalent:@""].target = actions;
    [m addItem:NSMenuItem.separatorItem];
}
