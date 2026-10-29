// Mac front end (front.h): AppKit owns the main thread; the window is 640x480 points (the console's 320x240 at 2x) and resizable, and its view's CAMetalLayer has a drawable
// of the view's size in pixels, presented again whenever the view needs drawing
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "front.h"
#include "present.h"
#include "ps1.h"

@interface FrontView : NSView
@end

@implementation FrontView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer { present_frame(); }
- (void)fit {
    CGFloat s = self.window ? self.window.backingScaleFactor : 1;
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    l.contentsScale = s; l.drawableSize = CGSizeMake(self.bounds.size.width * s, self.bounds.size.height * s);
    self.needsDisplay = YES;
}
- (void)setFrameSize:(NSSize)size { [super setFrameSize:size]; [self fit]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self fit]; }
@end

@interface FrontApp : NSObject <NSApplicationDelegate>
@end

@implementation FrontApp
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
@end

static FrontApp *g_app;   // NSApplication holds its delegate weakly
static NSWindow *g_win;

void front_run(const char *title) {
    NSApplication *app = NSApplication.sharedApplication;
    g_app = [FrontApp new]; app.delegate = g_app;
    app.activationPolicy = NSApplicationActivationPolicyRegular;   // a Dock icon and a menu bar, also when started from a terminal
    NSString *name = @(title);
    NSMenu *bar = [NSMenu new], *menu = [NSMenu new]; NSMenuItem *item = [NSMenuItem new];
    [menu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = menu; [bar addItem:item]; app.mainMenu = bar;
    g_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 480)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered defer:NO];
    g_win.title = name; g_win.releasedWhenClosed = NO;
    FrontView *v = [[FrontView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
    v.wantsLayer = YES; v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    g_win.contentView = v;
    if (!present_init((__bridge void *)v.layer)) RED("front end: no Metal device, the window stays empty");
    [v fit]; [g_win center]; [g_win makeKeyAndOrderFront:nil];
    [app activate];
    [app run];
}
